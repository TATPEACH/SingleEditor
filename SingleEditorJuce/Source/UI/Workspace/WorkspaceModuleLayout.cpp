#include "WorkspaceModuleLayout.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <juce_core/juce_core.h>

namespace
{
const char* moduleKey(WorkspaceModuleId id)
{
    switch (id)
    {
        case WorkspaceModuleId::coreWaveform: return "core-waveform";
        case WorkspaceModuleId::outputMeter: return "output-meter";
        case WorkspaceModuleId::ambisonicEnergy: return "ambisonic-energy";
        case WorkspaceModuleId::spatialEnergy: return "spatial-energy";
        case WorkspaceModuleId::parametricEq: return "parametric-eq";
        case WorkspaceModuleId::truePeakLimiter: return "true-peak-limiter";
        case WorkspaceModuleId::playlist: return "playlist";
        case WorkspaceModuleId::metadata: return "metadata";
        case WorkspaceModuleId::markers: return "markers";
    }

    return "unknown";
}

std::optional<WorkspaceModuleId> moduleIdForKey(const std::string& key)
{
    if (key == "output-meter")
        return WorkspaceModuleId::outputMeter;
    if (key == "ambisonic-energy")
        return WorkspaceModuleId::ambisonicEnergy;
    if (key == "spatial-energy")
        return WorkspaceModuleId::spatialEnergy;
    if (key == "parametric-eq")
        return WorkspaceModuleId::parametricEq;
    if (key == "true-peak-limiter")
        return WorkspaceModuleId::truePeakLimiter;
    if (key == "playlist")
        return WorkspaceModuleId::playlist;
    if (key == "metadata")
        return WorkspaceModuleId::metadata;
    if (key == "markers")
        return WorkspaceModuleId::markers;
    return std::nullopt;
}

std::vector<std::string> split(const std::string& value, char delimiter)
{
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter))
        parts.push_back(part);
    return parts;
}

bool parseInt(const std::string& text, int& value)
{
    if (text.empty())
        return false;

    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc {} && result.ptr == end;
}

constexpr auto workspacePresetSchema = "single-editor.workspace-preset";
constexpr int currentWorkspacePresetVersion = 2;
constexpr int maximumRowTracks = 256;
constexpr std::size_t maximumViewStateBytes = 8192;
constexpr int maximumViewStateDepth = 8;
constexpr int maximumViewStateNodes = 128;

juce::var makeObject()
{
    return juce::var(new juce::DynamicObject());
}

bool readIntProperty(const juce::DynamicObject& object, const juce::Identifier& name, int& value)
{
    const auto property = object.getProperty(name);
    if (!property.isInt() && !property.isInt64())
        return false;
    const auto converted = static_cast<juce::int64>(property);
    if (converted < std::numeric_limits<int>::min() || converted > std::numeric_limits<int>::max())
        return false;
    value = static_cast<int>(converted);
    return true;
}

std::optional<WorkspaceGridPlacement> placementFromVar(const juce::var& value)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        return std::nullopt;
    int column = 0;
    int row = 0;
    int columns = 0;
    int rows = 0;
    const auto* span = object->getProperty("span").getDynamicObject();
    if (span == nullptr
        || !readIntProperty(*object, "column", column)
        || !readIntProperty(*object, "row", row)
        || !readIntProperty(*span, "columns", columns)
        || !readIntProperty(*span, "rows", rows)
        || column < 0 || row < 0 || columns < 1 || rows < 1 || column + columns > 8)
    {
        return std::nullopt;
    }
    return WorkspaceGridPlacement { column, row, { columns, rows } };
}

juce::var placementToVar(const WorkspaceGridPlacement& placement)
{
    auto value = makeObject();
    auto* object = value.getDynamicObject();
    object->setProperty("column", placement.column);
    object->setProperty("row", placement.row);
    auto span = makeObject();
    span.getDynamicObject()->setProperty("columns", placement.span.columns);
    span.getDynamicObject()->setProperty("rows", placement.span.rows);
    object->setProperty("span", span);
    return value;
}

std::optional<WorkspaceLogicalBounds> logicalBoundsFromVar(const juce::var& value)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        return std::nullopt;

    WorkspaceLogicalBounds bounds;
    if (!readIntProperty(*object, "x", bounds.x)
        || !readIntProperty(*object, "y", bounds.y)
        || !readIntProperty(*object, "width", bounds.width)
        || !readIntProperty(*object, "height", bounds.height)
        || !bounds.isValid())
    {
        return std::nullopt;
    }
    return bounds;
}

juce::var logicalBoundsToVar(const WorkspaceLogicalBounds& bounds)
{
    auto value = makeObject();
    auto* object = value.getDynamicObject();
    object->setProperty("x", bounds.x);
    object->setProperty("y", bounds.y);
    object->setProperty("width", bounds.width);
    object->setProperty("height", bounds.height);
    return value;
}

std::optional<WorkspaceFloatingPlacement> floatingPlacementFromVar(
    const juce::DynamicObject& object)
{
    const auto bounds = logicalBoundsFromVar(object.getProperty("bounds"));
    const auto sourceWorkArea = logicalBoundsFromVar(object.getProperty("sourceWorkArea"));
    const auto monitorAffinity = object.getProperty("monitorAffinity").toString().toStdString();
    int scalePercent = 0;
    if (!bounds.has_value() || !sourceWorkArea.has_value() || monitorAffinity.empty()
        || !readIntProperty(object, "scalePercent", scalePercent)
        || scalePercent < 25 || scalePercent > 800)
    {
        return std::nullopt;
    }

    return WorkspaceFloatingPlacement { *bounds,
                                        *sourceWorkArea,
                                        monitorAffinity,
                                        scalePercent };
}

void writeFloatingPlacement(juce::DynamicObject& object,
                            const WorkspaceFloatingPlacement& placement)
{
    object.setProperty("bounds", logicalBoundsToVar(placement.bounds));
    object.setProperty("sourceWorkArea", logicalBoundsToVar(placement.sourceWorkArea));
    object.setProperty("monitorAffinity",
                       juce::String::fromUTF8(placement.monitorAffinity.c_str()));
    object.setProperty("scalePercent", placement.scalePercent);
}

bool validateJsonNode(const juce::var& value, int depth, int& nodeCount)
{
    if (depth > maximumViewStateDepth || ++nodeCount > maximumViewStateNodes)
        return false;
    if (const auto* object = value.getDynamicObject())
    {
        for (const auto& property : object->getProperties())
            if (!validateJsonNode(property.value, depth + 1, nodeCount))
                return false;
        return true;
    }
    if (const auto* array = value.getArray())
    {
        for (const auto& item : *array)
            if (!validateJsonNode(item, depth + 1, nodeCount))
                return false;
        return true;
    }
    return value.isVoid() || value.isUndefined() || value.isBool() || value.isInt()
        || value.isInt64() || value.isDouble() || value.isString();
}

bool validateViewState(const juce::var& value)
{
    if (value.getDynamicObject() == nullptr)
        return false;
    const auto encoded = juce::JSON::toString(value, true);
    if (encoded.getNumBytesAsUTF8() > static_cast<int>(maximumViewStateBytes))
        return false;
    int nodeCount = 0;
    return validateJsonNode(value, 0, nodeCount);
}

juce::var parseJsonObjectOrNew(const std::string& text)
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
}

bool WorkspaceGridPlacement::overlaps(const WorkspaceGridPlacement& other) const noexcept
{
    return column < other.right() && right() > other.column
        && row < other.bottom() && bottom() > other.row;
}

void WorkspaceModuleLayout::registerModule(WorkspaceModuleDescriptor descriptor)
{
    if (descriptor.id == WorkspaceModuleId::coreWaveform || findEntry(descriptor.id) != nullptr)
        return;

    if (descriptor.instanceId.empty())
        descriptor.instanceId = moduleKey(descriptor.id);
    if (descriptor.moduleTypeId.empty())
        descriptor.moduleTypeId = moduleKey(descriptor.id);

    descriptor.defaultWeight = std::max(1, descriptor.defaultWeight);
    descriptor.minimumExtent = std::max(0, descriptor.minimumExtent);
    descriptor.minimumSpan.columns = std::clamp(descriptor.minimumSpan.columns, 1, gridColumns);
    descriptor.minimumSpan.rows = std::max(1, descriptor.minimumSpan.rows);
    descriptor.preferredSpan.columns = std::clamp(descriptor.preferredSpan.columns,
                                                  descriptor.minimumSpan.columns,
                                                  gridColumns);
    descriptor.preferredSpan.rows = std::max(descriptor.minimumSpan.rows,
                                             descriptor.preferredSpan.rows);
    // A Mag's information density may have a preferred sweet spot, but its
    // outer rectangle belongs to the workspace split model.  Every Mag must
    // therefore be able to span any available track and adapt its contents.
    descriptor.maximumSpan.columns = gridColumns;
    // Vertical group splits can create more than eight logical rows. Keep the
    // outer span compatible with the persisted row-track capacity; the old
    // display-density limit made valid saved layouts fail on the next launch.
    descriptor.maximumSpan.rows = std::max({ descriptor.preferredSpan.rows,
                                             descriptor.maximumSpan.rows,
                                             maximumRowTracks });

    const WorkspaceGridPlacement defaultPlacement {
        descriptor.defaultGridColumn < 0
            ? gridColumns - descriptor.preferredSpan.columns
            : std::clamp(descriptor.defaultGridColumn,
                         0,
                         gridColumns - descriptor.preferredSpan.columns),
        std::max(0, descriptor.defaultGridRow),
        descriptor.preferredSpan
    };
    entries.push_back({ descriptor,
                        false,
                        true,
                        descriptor.defaultPlaced,
                        descriptor.defaultOrder,
                        descriptor.defaultWeight,
                        defaultPlacement,
                        false });
}

void WorkspaceModuleLayout::setAvailable(WorkspaceModuleId id, bool available)
{
    if (auto* entry = findEntry(id))
    {
        entry->available = available;
        if (!available)
            replaceInactiveTabIfNeeded(*entry);
        ensureActivePlacements();
        reflowDefaultPlacements();
        fillAllGridHoles();
    }
}

void WorkspaceModuleLayout::setEnabled(WorkspaceModuleId id, bool enabled)
{
    if (auto* entry = findEntry(id))
    {
        entry->enabled = enabled;
        if (!enabled)
        {
            removeFloatingTileForModule(id);
            replaceInactiveTabIfNeeded(*entry);
        }
        ensureActivePlacements();
        reflowDefaultPlacements();
        fillAllGridHoles();
    }
}

WorkspaceModuleLayout WorkspaceModuleLayout::withCoreOnlyPresentation() const
{
    auto projected = *this;
    // This copy is only a view. Canonical placement, availability, processing,
    // view state and history remain owned by the original layout.
    for (auto& entry : projected.entries)
    {
        entry.available = false;
        entry.tabStackId.clear();
    }
    projected.corePlacement = { 0, 0, { 1, 1 } };
    projected.stacks.clear();
    projected.inert.clear();
    projected.floating.clear();
    projected.opaqueFloatingRecords.clear();
    projected.columnTrackWeights.clear();
    projected.rowTrackWeights.clear();
    return projected;
}

bool WorkspaceModuleLayout::isAvailable(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->available;
    return false;
}

bool WorkspaceModuleLayout::isEnabled(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->enabled;
    return false;
}

bool WorkspaceModuleLayout::isPlaced(WorkspaceModuleId id) const
{
    if (id == WorkspaceModuleId::coreWaveform)
        return true;
    if (const auto* entry = findEntry(id))
        return entry->placed;
    return false;
}

bool WorkspaceModuleLayout::isActive(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->available && entry->enabled && entry->placed;
    return false;
}

bool WorkspaceModuleLayout::isModuleVisible(WorkspaceModuleId id) const
{
    const auto* entry = findEntry(id);
    if (entry == nullptr || !entry->available || !entry->enabled || !entry->placed)
        return false;
    if (entry->tabStackId.empty())
        return true;
    if (const auto* stack = findTabStack(entry->tabStackId))
        return stack->activeInstanceId == entry->descriptor.instanceId;
    return true;
}

std::vector<WorkspaceModuleId> WorkspaceModuleLayout::activeModules() const
{
    std::vector<WorkspaceModuleId> result;
    for (const auto* entry : entriesInOrder())
        if (entry->available && entry->enabled && entry->placed)
            result.push_back(entry->descriptor.id);
    return result;
}

std::vector<WorkspaceModuleId> WorkspaceModuleLayout::visibleModules() const
{
    std::vector<WorkspaceModuleId> result;
    for (const auto* entry : entriesInOrder())
        if (isModuleVisible(entry->descriptor.id))
            result.push_back(entry->descriptor.id);
    return result;
}

void WorkspaceModuleLayout::moveModuleToActiveIndex(WorkspaceModuleId id, int activeIndex)
{
    auto ordered = entriesInOrder();
    const auto moving = std::find_if(ordered.begin(), ordered.end(), [id] (const Entry* entry)
    {
        return entry->descriptor.id == id;
    });
    if (moving == ordered.end() || !(*moving)->available || !(*moving)->enabled || !(*moving)->placed)
        return;

    auto* movingEntry = *moving;
    ordered.erase(moving);

    std::vector<Entry*> active;
    for (auto* entry : ordered)
        if (entry->available && entry->enabled && entry->placed)
            active.push_back(entry);

    activeIndex = std::clamp(activeIndex, 0, static_cast<int>(active.size()));
    auto insertion = ordered.end();
    if (activeIndex < static_cast<int>(active.size()))
        insertion = std::find(ordered.begin(), ordered.end(), active[static_cast<size_t>(activeIndex)]);

    ordered.insert(insertion, movingEntry);
    for (size_t index = 0; index < ordered.size(); ++index)
        ordered[index]->order = static_cast<int>(index);
}

int WorkspaceModuleLayout::gridColumnCount() const noexcept
{
    return gridColumns;
}

int WorkspaceModuleLayout::requiredGridRows() const
{
    auto rows = corePlacement.bottom();
    for (const auto& entry : entries)
        if (entry.available && entry.enabled && entry.placed && !entryIsFloating(entry))
            rows = std::max(rows, entry.placement.bottom());
    for (const auto& module : inert)
        if (module.enabled && module.placed)
            rows = std::max(rows, module.placement.bottom());
    return std::max(1, rows);
}

std::vector<WorkspaceGridItem> WorkspaceModuleLayout::gridItems() const
{
    std::vector<WorkspaceGridItem> result;
    result.push_back({ WorkspaceModuleId::coreWaveform, corePlacement, true });

    std::vector<const Entry*> active;
    for (const auto& entry : entries)
        if (entry.available && entry.enabled && entry.placed && !entryIsFloating(entry))
            active.push_back(&entry);
    std::stable_sort(active.begin(), active.end(), [] (const Entry* left, const Entry* right)
    {
        return std::tie(left->placement.row, left->placement.column, left->order)
             < std::tie(right->placement.row, right->placement.column, right->order);
    });
    std::unordered_set<std::string> emittedStacks;
    for (const auto* entry : active)
    {
        if (entry->tabStackId.empty())
        {
            result.push_back({ entry->descriptor.id, entry->placement, false });
            continue;
        }
        if (!emittedStacks.insert(entry->tabStackId).second)
            continue;
        if (const auto* stack = findTabStack(entry->tabStackId))
        {
            auto activeId = entry->descriptor.id;
            if (const auto* activeEntry = findEntryByInstanceId(stack->activeInstanceId))
                activeId = activeEntry->descriptor.id;
            std::vector<WorkspaceModuleId> knownTabs;
            for (const auto& instanceId : stack->tabInstanceIds)
                if (const auto* member = findEntryByInstanceId(instanceId))
                    knownTabs.push_back(member->descriptor.id);
            result.push_back({ activeId, stack->placement, false, stack->id, std::move(knownTabs) });
        }
    }
    return result;
}

std::optional<WorkspaceGridPlacement> WorkspaceModuleLayout::getGridPlacement(WorkspaceModuleId id) const
{
    if (id == WorkspaceModuleId::coreWaveform)
        return corePlacement;
    if (const auto* entry = findEntry(id))
        return entry->placement;
    return std::nullopt;
}

WorkspaceGridMutationResult WorkspaceModuleLayout::setModulePlaced(WorkspaceModuleId id, bool placed)
{
    if (id == WorkspaceModuleId::coreWaveform)
        return placed ? WorkspaceGridMutationResult::noChange
                      : WorkspaceGridMutationResult::protectedCore;

    auto* entry = findEntry(id);
    if (entry == nullptr)
        return WorkspaceGridMutationResult::unknownModule;
    if (placed && !entry->available)
        return WorkspaceGridMutationResult::moduleUnavailable;
    if (entry->placed == placed)
        return WorkspaceGridMutationResult::noChange;

    const auto before = captureUserState();
    setModulePlacedInternal(*entry, placed);
    return commitMutation(before);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::setModuleOpen(WorkspaceModuleId id, bool open)
{
    if (id == WorkspaceModuleId::coreWaveform)
        return open ? WorkspaceGridMutationResult::noChange
                    : WorkspaceGridMutationResult::protectedCore;

    auto* entry = findEntry(id);
    if (entry == nullptr)
        return WorkspaceGridMutationResult::unknownModule;
    if (open && !entry->available)
        return WorkspaceGridMutationResult::moduleUnavailable;
    if (entry->enabled == open && entry->placed == open)
        return WorkspaceGridMutationResult::noChange;

    const auto before = captureUserState();
    const auto trackSnapshot = captureTrackWeightSnapshot();
    if (open && lastClosedDock && lastClosedDock->id == id
        && userStateEquals(before, lastClosedDock->afterClose)
        && std::equal(before.entries.begin(), before.entries.end(),
                      lastClosedDock->afterClose.entries.begin(),
                      [] (const Entry& current, const Entry& closed)
                      { return current.available == closed.available; }))
    {
        // Closing fills the vacated tile, so reinserting the old rectangle
        // alone would overlap its neighbours. Restore the complete split
        // geometry only while the post-close state is still unchanged.
        restoreUserState(lastClosedDock->beforeClose);
        lastClosedDock.reset();
        return commitMutation(before);
    }
    const auto rememberDock = !open && entry->available && entry->enabled
                           && entry->placed && !entryIsFloating(*entry);
    if (open)
    {
        entry->enabled = true;
        if (!entry->placed)
        {
            const auto hasCustomTopology = !stacks.empty()
                || !columnTrackWeights.empty() || !rowTrackWeights.empty()
                || std::any_of(entries.begin(), entries.end(), [entry, this] (const Entry& candidate)
                {
                    return &candidate != entry && candidate.gridCustomized
                        && candidate.available && candidate.enabled && candidate.placed
                        && !entryIsFloating(candidate);
                });
            if (hasCustomTopology)
            {
                entry->placed = true;
                entry->tabStackId.clear();
                placeNewModuleBelowRoot(*entry);
                fillAllGridHoles();
            }
            else
            {
                setModulePlacedInternal(*entry, true);
            }
        }
        else
        {
            ensureActivePlacements();
            reflowDefaultPlacements();
            fillAllGridHoles();
        }
    }
    else
    {
        if (entry->placed)
            setModulePlacedInternal(*entry, false);
        entry->enabled = false;
        reflowDefaultPlacements();
        fillAllGridHoles();
    }
    preserveUnchangedTrackWeights(trackSnapshot);
    const auto result = commitMutation(before);
    if (result == WorkspaceGridMutationResult::committed)
    {
        if (rememberDock)
            lastClosedDock = ClosedDock { id, before, captureUserState() };
        else
            lastClosedDock.reset();
    }
    return result;
}

void WorkspaceModuleLayout::setModulePlacedInternal(Entry& entry, bool placed)
{
    const auto trackSnapshot = captureTrackWeightSnapshot();
    const auto id = entry.descriptor.id;
    const auto oldPlacement = entry.placement;
    std::optional<FloatingTile> floatingStackBefore;
    std::vector<std::string> floatingStackMembers;
    if (!placed && !entry.tabStackId.empty())
    {
        if (const auto* tile = findFloatingTileForModule(id))
            floatingStackBefore = *tile;
        if (const auto* stack = findTabStack(entry.tabStackId))
            floatingStackMembers = stack->tabInstanceIds;
    }
    entry.placed = placed;
    if (placed)
        ensureActivePlacements();
    else
    {
        removeFloatingTileForModule(id);
        removeModuleFromTabStack(id);
        if (floatingStackBefore.has_value())
        {
            auto replacementId = floatingStackBefore->id;
            if (findTabStack(replacementId) == nullptr)
            {
                replacementId.clear();
                for (const auto& instanceId : floatingStackMembers)
                {
                    if (instanceId == entry.descriptor.instanceId)
                        continue;
                    if (const auto* remaining = findEntryByInstanceId(instanceId);
                        remaining != nullptr && remaining->available
                            && remaining->enabled && remaining->placed)
                    {
                        replacementId = instanceId;
                        break;
                    }
                }
            }
            if (!replacementId.empty())
            {
                floatingStackBefore->id = replacementId;
                floating.push_back(std::move(*floatingStackBefore));
            }
        }
        const auto stillOccupied = std::any_of(entries.begin(), entries.end(), [this, &oldPlacement] (const Entry& candidate)
        {
            return candidate.available && candidate.enabled && candidate.placed
                && !entryIsFloating(candidate) && candidate.placement == oldPlacement;
        });
        if (!stillOccupied)
        {
            fillVacatedTile(oldPlacement, id);
            compactGridTracks(id);
        }
    }
    reflowDefaultPlacements();
    fillAllGridHoles();
    preserveUnchangedTrackWeights(trackSnapshot);
}

void WorkspaceModuleLayout::placeNewModuleBelowRoot(Entry& entry)
{
    const auto items = gridItems();
    auto left = gridColumns;
    auto right = 0;
    auto bottom = 0;
    for (const auto& item : items)
    {
        if (item.id == entry.descriptor.id)
            continue;
        left = std::min(left, item.placement.column);
        right = std::max(right, item.placement.right());
        bottom = std::max(bottom, item.placement.bottom());
    }
    if (right <= left)
    {
        left = 0;
        right = std::max(2, corePlacement.right());
        bottom = corePlacement.bottom();
    }
    entry.placement = { left, bottom, { right - left, 1 } };
    entry.gridCustomized = true;
}

WorkspaceGridMutationResult WorkspaceModuleLayout::moveModule(WorkspaceModuleId id,
                                                               WorkspaceGridPlacement placement,
                                                               WorkspaceGridDirection direction)
{
    if (isFloating(id))
        return WorkspaceGridMutationResult::invalidPlacement;
    const auto before = captureUserState();
    WorkspaceGridPlacement oldPlacement;
    if (id == WorkspaceModuleId::coreWaveform)
    {
        oldPlacement = corePlacement;
        placement.span = corePlacement.span;
        placement.column = std::clamp(placement.column, 0, gridColumns - placement.span.columns);
        placement.row = std::max(0, placement.row);
        if (placement == corePlacement)
            return WorkspaceGridMutationResult::noChange;
        corePlacement = placement;
    }
    else
    {
        auto* entry = findEntry(id);
        if (entry == nullptr)
            return WorkspaceGridMutationResult::unknownModule;
        if (!entry->available || !entry->enabled || !entry->placed)
            return WorkspaceGridMutationResult::moduleUnavailable;
        oldPlacement = entry->placement;
        placement.span = entry->placement.span;
        placement = normalisePlacement(*entry, placement);
        if (placement == entry->placement)
            return WorkspaceGridMutationResult::noChange;
        if (placement.overlaps(corePlacement))
            return WorkspaceGridMutationResult::protectedCore;
        setTilePlacement(id, placement);
        setTileCustomized(id);
    }

    if (!resolveCollisions(id, direction))
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::noSpace;
    }

    fillLocalHole(oldPlacement, id);
    return commitMutation(before);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::resizeModule(WorkspaceModuleId id,
                                                                 WorkspaceGridSpan span,
                                                                 WorkspaceGridDirection direction)
{
    if (isFloating(id))
        return WorkspaceGridMutationResult::invalidPlacement;
    const auto before = captureUserState();
    if (id == WorkspaceModuleId::coreWaveform)
    {
        span.columns = std::clamp(span.columns, 2, gridColumns);
        span.rows = std::max(1, span.rows);
        auto candidate = corePlacement;
        candidate.span = span;
        candidate.column = std::clamp(candidate.column, 0, gridColumns - span.columns);
        if (candidate == corePlacement)
            return WorkspaceGridMutationResult::noChange;
        corePlacement = candidate;
    }
    else
    {
        auto* entry = findEntry(id);
        if (entry == nullptr)
            return WorkspaceGridMutationResult::unknownModule;
        if (!entry->available || !entry->enabled || !entry->placed)
            return WorkspaceGridMutationResult::moduleUnavailable;

        auto candidate = entry->placement;
        candidate.span = span;
        candidate = normalisePlacement(*entry, candidate);
        if (candidate == entry->placement)
            return WorkspaceGridMutationResult::noChange;
        if (candidate.overlaps(corePlacement))
            return WorkspaceGridMutationResult::protectedCore;
        setTilePlacement(id, candidate);
        setTileCustomized(id);
    }

    if (!resolveCollisions(id, direction))
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::noSpace;
    }
    return commitMutation(before);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::dockModule(WorkspaceModuleId id,
                                                               WorkspaceModuleId target,
                                                               WorkspaceDockZone zone)
{
    if (isFloating(id) || isFloating(target))
        return WorkspaceGridMutationResult::invalidPlacement;
    if (zone == WorkspaceDockZone::centre)
    {
        if (target == WorkspaceModuleId::coreWaveform || id == WorkspaceModuleId::coreWaveform)
            return WorkspaceGridMutationResult::protectedCore;
        return createOrMergeTabStack(id, target);
    }
    return dockModuleToGroup(id, { target }, zone);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::dockModuleToGroup(
    WorkspaceModuleId id,
    std::vector<WorkspaceModuleId> targets,
    WorkspaceDockZone zone)
{
    if (zone == WorkspaceDockZone::centre)
        return targets.size() == 1
            ? dockModule(id, targets.front(), zone)
            : WorkspaceGridMutationResult::invalidPlacement;
    if (id == WorkspaceModuleId::coreWaveform)
        return WorkspaceGridMutationResult::protectedCore;
    if (isFloating(id))
        return WorkspaceGridMutationResult::invalidPlacement;

    std::vector<WorkspaceModuleId> uniqueTargets;
    uniqueTargets.reserve(targets.size());
    for (const auto target : targets)
    {
        if (target == id)
            continue;
        if (std::find(uniqueTargets.begin(), uniqueTargets.end(), target) == uniqueTargets.end())
            uniqueTargets.push_back(target);
    }
    if (uniqueTargets.empty())
        return WorkspaceGridMutationResult::invalidPlacement;

    auto* moving = findEntry(id);
    if (moving == nullptr)
        return WorkspaceGridMutationResult::unknownModule;
    if (!moving->available || !moving->enabled || !moving->placed)
        return WorkspaceGridMutationResult::moduleUnavailable;
    for (const auto target : uniqueTargets)
    {
        if (isFloating(target))
            return WorkspaceGridMutationResult::invalidPlacement;
        if (target == WorkspaceModuleId::coreWaveform)
            continue;
        const auto* targetEntry = findEntry(target);
        if (targetEntry == nullptr)
            return WorkspaceGridMutationResult::unknownModule;
        if (!targetEntry->available || !targetEntry->enabled || !targetEntry->placed)
            return WorkspaceGridMutationResult::moduleUnavailable;
    }
    if (uniqueTargets.size() == 1
        && uniqueTargets.front() == WorkspaceModuleId::coreWaveform)
    {
        const auto& placement = moving->placement;
        const auto alreadyOnRequestedEdge = zone == WorkspaceDockZone::left
            ? placement.right() <= corePlacement.column
            : zone == WorkspaceDockZone::right
                ? placement.column >= corePlacement.right()
                : zone == WorkspaceDockZone::top
                    ? placement.bottom() <= corePlacement.row
                    : placement.row >= corePlacement.bottom();
        if (alreadyOnRequestedEdge)
            return WorkspaceGridMutationResult::noChange;
    }

    const auto before = captureUserState();
    const auto sourcePlacement = moving->placement;
    if (!moving->tabStackId.empty())
        removeModuleFromTabStack(id);

    // Remove the source hole before adding the new split.  A neighbouring tile
    // consumes it, and wholly unused logical tracks are compacted.  This is the
    // no-blank invariant of the workspace rather than a visual patch in Host.
    fillVacatedTile(sourcePlacement, id);
    compactGridTracks(id);
    const auto splitSnapshot = captureTrackWeightSnapshot();

    auto items = gridItems();
    std::vector<const WorkspaceGridItem*> targetItems;
    for (const auto target : uniqueTargets)
    {
        const auto found = std::find_if(items.begin(), items.end(), [target] (const auto& item)
        {
            return item.id == target
                || std::find(item.tabMembers.begin(), item.tabMembers.end(), target)
                       != item.tabMembers.end();
        });
        if (found == items.end())
        {
            restoreUserState(before);
            return WorkspaceGridMutationResult::invalidPlacement;
        }
        if (std::find(targetItems.begin(), targetItems.end(), &*found) == targetItems.end())
            targetItems.push_back(&*found);
    }

    auto groupRegion = targetItems.front()->placement;
    for (const auto* item : targetItems)
    {
        const auto left = std::min(groupRegion.column, item->placement.column);
        const auto top = std::min(groupRegion.row, item->placement.row);
        const auto right = std::max(groupRegion.right(), item->placement.right());
        const auto bottom = std::max(groupRegion.bottom(), item->placement.bottom());
        groupRegion = { left, top, { right - left, bottom - top } };
    }
    for (auto row = groupRegion.row; row < groupRegion.bottom(); ++row)
        for (auto column = groupRegion.column; column < groupRegion.right(); ++column)
        {
            const auto covered = std::any_of(targetItems.begin(), targetItems.end(),
                                             [column, row] (const auto* item)
            {
                return column >= item->placement.column && column < item->placement.right()
                    && row >= item->placement.row && row < item->placement.bottom();
            });
            if (!covered)
            {
                restoreUserState(before);
                return WorkspaceGridMutationResult::invalidPlacement;
            }
        }

    const auto horizontalSplit = zone == WorkspaceDockZone::left
                              || zone == WorkspaceDockZone::right;
    const auto insertion = horizontalSplit
        ? (zone == WorkspaceDockZone::left ? groupRegion.column : groupRegion.right())
        : (zone == WorkspaceDockZone::top ? groupRegion.row : groupRegion.bottom());
    auto maximumRight = 0;
    for (const auto& item : items)
        if (item.id != id)
            maximumRight = std::max(maximumRight, item.placement.right());
    if (horizontalSplit && maximumRight >= gridColumns)
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::noSpace;
    }

    const auto shiftPlacement = [horizontalSplit, insertion] (WorkspaceGridPlacement placement)
    {
        if (horizontalSplit)
        {
            if (placement.column >= insertion)
                ++placement.column;
            else if (placement.right() > insertion)
                ++placement.span.columns;
        }
        else
        {
            if (placement.row >= insertion)
                ++placement.row;
            else if (placement.bottom() > insertion)
                ++placement.span.rows;
        }
        return placement;
    };

    for (const auto& item : items)
    {
        if (item.id == id)
            continue;
        const auto shifted = shiftPlacement(item.placement);
        if (item.id == WorkspaceModuleId::coreWaveform)
            corePlacement = shifted;
        else
            setTilePlacement(item.id, shifted);
    }
    for (auto& module : inert)
        if (module.enabled && module.placed)
            module.placement = shiftPlacement(module.placement);

    const auto destination = horizontalSplit
        ? WorkspaceGridPlacement { insertion, groupRegion.row, { 1, groupRegion.span.rows } }
        : WorkspaceGridPlacement { groupRegion.column, insertion, { groupRegion.span.columns, 1 } };
    setTilePlacement(id, destination);
    setTileCustomized(id);
    fillAllGridHoles();

    // A new split takes its space only from the target group.  Tracks outside
    // that group retain their user-resized pixel extents even though this edit
    // adds one projected track to the same axis (for example when a tab tears
    // out beside Output while a narrow EQ remains on the far edge).
    preserveTrackWeightsAfterSplit(splitSnapshot, groupRegion, zone);
    return commitMutation(before);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::activateTab(WorkspaceModuleId id)
{
    auto* entry = findEntry(id);
    if (entry == nullptr)
        return WorkspaceGridMutationResult::unknownModule;
    if (!entry->available || !entry->enabled || !entry->placed)
        return WorkspaceGridMutationResult::moduleUnavailable;
    return activateTabInstanceId(entry->descriptor.instanceId);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::activateTabInstanceId(const std::string& id)
{
    const auto known = findEntryByInstanceId(id);
    if (known != nullptr && (!known->available || !known->enabled || !known->placed))
        return WorkspaceGridMutationResult::moduleUnavailable;
    const auto inertEntry = std::find_if(inert.begin(), inert.end(), [&id] (const InertModule& item)
    {
        return item.instanceId == id && item.enabled && item.placed;
    });
    if (known == nullptr && inertEntry == inert.end())
        return WorkspaceGridMutationResult::unknownModule;
    const auto stackId = known != nullptr ? known->tabStackId : inertEntry->tabStackId;
    auto* stack = findTabStack(stackId);
    if (stack == nullptr)
        return WorkspaceGridMutationResult::noChange;
    if (stack->activeInstanceId == id)
        return WorkspaceGridMutationResult::noChange;
    if (std::find(stack->tabInstanceIds.begin(), stack->tabInstanceIds.end(), id)
        == stack->tabInstanceIds.end())
        return WorkspaceGridMutationResult::unknownModule;

    const auto before = captureUserState();
    stack->activeInstanceId = id;
    return commitMutation(before);
}

std::vector<WorkspaceTabStackInfo> WorkspaceModuleLayout::tabStacks() const
{
    std::vector<WorkspaceTabStackInfo> result;
    result.reserve(stacks.size());
    for (const auto& stack : stacks)
    {
        WorkspaceTabStackInfo info;
        info.id = stack.id;
        info.placement = stack.placement;
        info.tabInstanceIds = stack.tabInstanceIds;
        info.activeInstanceId = stack.activeInstanceId;
        for (const auto& instanceId : stack.tabInstanceIds)
            if (const auto* member = findEntryByInstanceId(instanceId))
                info.tabs.push_back(member->descriptor.id);
        if (const auto* active = findEntryByInstanceId(stack.activeInstanceId))
            info.activeTab = active->descriptor.id;
        result.push_back(std::move(info));
    }
    return result;
}

std::optional<WorkspaceTabStackInfo> WorkspaceModuleLayout::tabStackForModule(WorkspaceModuleId id) const
{
    const auto* entry = findEntry(id);
    if (entry == nullptr || entry->tabStackId.empty())
        return std::nullopt;
    if (const auto* stack = findTabStack(entry->tabStackId))
    {
        WorkspaceTabStackInfo info;
        info.id = stack->id;
        info.placement = stack->placement;
        info.tabInstanceIds = stack->tabInstanceIds;
        info.activeInstanceId = stack->activeInstanceId;
        for (const auto& instanceId : stack->tabInstanceIds)
            if (const auto* member = findEntryByInstanceId(instanceId))
                info.tabs.push_back(member->descriptor.id);
        if (const auto* active = findEntryByInstanceId(stack->activeInstanceId))
            info.activeTab = active->descriptor.id;
        return info;
    }
    return std::nullopt;
}

std::optional<WorkspaceModuleId> WorkspaceModuleLayout::moduleForInstanceId(const std::string& id) const
{
    if (const auto* entry = findEntryByInstanceId(id))
        return entry->descriptor.id;
    return std::nullopt;
}

std::vector<WorkspaceInertModuleInfo> WorkspaceModuleLayout::inertModules() const
{
    std::vector<WorkspaceInertModuleInfo> result;
    result.reserve(inert.size());
    for (const auto& module : inert)
    {
        if (module.enabled && module.placed)
            result.push_back({ module.instanceId,
                               module.moduleTypeId,
                               module.placement,
                               module.tabStackId,
                               module.quarantined });
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleLayout::floatModule(
    WorkspaceModuleId id,
    WorkspaceFloatingPlacement placement)
{
    if (!placement.bounds.isValid() || !placement.sourceWorkArea.isValid()
        || placement.scalePercent < 25 || placement.scalePercent > 800)
    {
        return WorkspaceGridMutationResult::invalidPlacement;
    }
    if (id != WorkspaceModuleId::coreWaveform && !isActive(id))
        return findEntry(id) == nullptr ? WorkspaceGridMutationResult::unknownModule
                                        : WorkspaceGridMutationResult::moduleUnavailable;

    const auto before = captureUserState();
    if (auto* existing = findFloatingTileForModule(id))
    {
        if (existing->placement == placement)
            return WorkspaceGridMutationResult::noChange;
        existing->placement = std::move(placement);
        return commitMutation(before);
    }

    floating.push_back({ tileIdForModule(id),
                         std::move(placement),
                         id == WorkspaceModuleId::coreWaveform });
    reflowDefaultPlacements();
    return commitMutation(before);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::redockModule(WorkspaceModuleId id)
{
    if (!isFloating(id))
        return WorkspaceGridMutationResult::noChange;
    const auto before = captureUserState();
    removeFloatingTileForModule(id);
    ensureActivePlacements();
    if (id != WorkspaceModuleId::coreWaveform
        && !resolveCollisions(id, WorkspaceGridDirection::none))
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::noSpace;
    }
    return commitMutation(before);
}

WorkspaceGridMutationResult WorkspaceModuleLayout::removeFloatingTile(WorkspaceModuleId id)
{
    const auto* tile = findFloatingTileForModule(id);
    if (tile == nullptr)
        return WorkspaceGridMutationResult::noChange;
    if (tile->privilegedCore)
        return WorkspaceGridMutationResult::protectedCore;

    const auto before = captureUserState();
    const auto tileId = tile->id;
    if (const auto* stack = findTabStack(tileId))
    {
        const auto members = stack->tabInstanceIds;
        for (const auto& instanceId : members)
        {
            if (auto* entry = findEntryByInstanceId(instanceId))
            {
                entry->placed = false;
                entry->tabStackId.clear();
            }
            else if (auto opaque = std::find_if(inert.begin(), inert.end(), [&] (const InertModule& module)
                     {
                         return module.instanceId == instanceId;
                     }); opaque != inert.end())
            {
                opaque->placed = false;
                opaque->tabStackId.clear();
            }
        }
        stacks.erase(std::remove_if(stacks.begin(), stacks.end(), [&] (const TabStack& item)
        {
            return item.id == tileId;
        }), stacks.end());
    }
    else if (auto* entry = findEntryByInstanceId(tileId))
    {
        entry->placed = false;
        entry->tabStackId.clear();
    }
    else
    {
        return WorkspaceGridMutationResult::unknownModule;
    }

    floating.erase(std::remove_if(floating.begin(), floating.end(), [&] (const FloatingTile& item)
    {
        return item.id == tileId;
    }), floating.end());
    ensureActivePlacements();
    reflowDefaultPlacements();
    return commitMutation(before);
}

bool WorkspaceModuleLayout::updateFloatingPlacement(
    WorkspaceModuleId id,
    WorkspaceFloatingPlacement placement)
{
    auto* tile = findFloatingTileForModule(id);
    if (tile == nullptr || !placement.bounds.isValid() || !placement.sourceWorkArea.isValid()
        || placement.scalePercent < 25 || placement.scalePercent > 800)
    {
        return false;
    }
    tile->placement = std::move(placement);
    return true;
}

bool WorkspaceModuleLayout::isFloating(WorkspaceModuleId id) const
{
    return findFloatingTileForModule(id) != nullptr;
}

std::optional<WorkspaceFloatingTileInfo> WorkspaceModuleLayout::floatingTileForModule(
    WorkspaceModuleId id) const
{
    const auto* tile = findFloatingTileForModule(id);
    if (tile == nullptr)
        return std::nullopt;
    for (const auto& info : floatingTiles())
        if (info.id == tile->id)
            return info;
    return std::nullopt;
}

std::vector<WorkspaceFloatingTileInfo> WorkspaceModuleLayout::floatingTiles() const
{
    std::vector<WorkspaceFloatingTileInfo> result;
    result.reserve(floating.size());
    for (const auto& tile : floating)
    {
        WorkspaceFloatingTileInfo info;
        info.id = tile.id;
        info.privilegedCore = tile.privilegedCore;
        info.placement = tile.placement;
        if (tile.privilegedCore)
        {
            info.members.push_back(WorkspaceModuleId::coreWaveform);
        }
        else if (const auto* stack = findTabStack(tile.id))
        {
            for (const auto& instanceId : stack->tabInstanceIds)
                if (const auto* entry = findEntryByInstanceId(instanceId))
                    info.members.push_back(entry->descriptor.id);
        }
        else if (const auto* entry = findEntryByInstanceId(tile.id))
        {
            info.members.push_back(entry->descriptor.id);
        }
        if (!info.members.empty())
            result.push_back(std::move(info));
    }
    return result;
}

std::size_t WorkspaceModuleLayout::estimatedViewBytes(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->descriptor.estimatedViewBytes;
    return 0;
}

bool WorkspaceModuleLayout::allowsInactiveViewResidency(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->descriptor.allowInactiveViewResidency;
    return false;
}

bool WorkspaceModuleLayout::setModuleViewState(WorkspaceModuleId id,
                                                int schemaVersion,
                                                std::string jsonPayload)
{
    auto* entry = findEntry(id);
    juce::var payload;
    if (entry == nullptr || schemaVersion < 1 || jsonPayload.size() > maximumViewStateBytes
        || juce::JSON::parse(juce::String::fromUTF8(jsonPayload.c_str()), payload).failed()
        || !validateViewState(payload))
        return false;
    entry->viewStateVersion = schemaVersion;
    entry->viewStateJson = std::move(jsonPayload);
    return true;
}

std::string WorkspaceModuleLayout::getModuleViewState(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->viewStateJson;
    return {};
}

bool WorkspaceModuleLayout::canUndoGridMutation() const noexcept
{
    return !undoHistory.empty();
}

bool WorkspaceModuleLayout::canRedoGridMutation() const noexcept
{
    return !redoHistory.empty();
}

bool WorkspaceModuleLayout::undoGridMutation()
{
    if (undoHistory.empty())
        return false;
    const auto current = captureUserState();
    const auto previous = undoHistory.back();
    undoHistory.pop_back();
    redoHistory.push_back(current);
    restoreUserState(previous);
    return true;
}

bool WorkspaceModuleLayout::redoGridMutation()
{
    if (redoHistory.empty())
        return false;
    const auto current = captureUserState();
    const auto next = redoHistory.back();
    redoHistory.pop_back();
    undoHistory.push_back(current);
    restoreUserState(next);
    return true;
}

size_t WorkspaceModuleLayout::gridUndoDepth() const noexcept
{
    return undoHistory.size();
}

void WorkspaceModuleLayout::setWeight(WorkspaceModuleId id, int weight)
{
    if (auto* entry = findEntry(id))
        entry->weight = std::max(1, weight);
}

int WorkspaceModuleLayout::getWeight(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->weight;
    return 0;
}

int WorkspaceModuleLayout::getMinimumExtent(WorkspaceModuleId id) const
{
    if (const auto* entry = findEntry(id))
        return entry->descriptor.minimumExtent;
    return 0;
}

std::vector<int> WorkspaceModuleLayout::calculateActiveExtents(int totalExtent, int gap) const
{
    std::vector<const Entry*> active;
    for (const auto* entry : entriesInOrder())
        if (entry->available && entry->enabled && entry->placed && !entryIsFloating(*entry))
            active.push_back(entry);

    if (active.empty())
        return {};

    const auto gapTotal = std::max(0, gap) * static_cast<int>(active.size() - 1);
    const auto availableExtent = std::max(0, totalExtent - gapTotal);
    std::vector<int> extents(active.size(), 0);

    int minimumTotal = 0;
    for (const auto* entry : active)
        minimumTotal += entry->descriptor.minimumExtent;

    if (availableExtent < minimumTotal)
    {
        int assigned = 0;
        for (size_t index = 0; index < active.size(); ++index)
        {
            const auto extent = index + 1 == active.size()
                ? availableExtent - assigned
                : static_cast<int>((static_cast<long long>(availableExtent)
                                    * active[index]->descriptor.minimumExtent) / std::max(1, minimumTotal));
            extents[index] = std::max(0, extent);
            assigned += extents[index];
        }
        return extents;
    }

    long long weightTotal = 0;
    for (size_t index = 0; index < active.size(); ++index)
    {
        extents[index] = active[index]->descriptor.minimumExtent;
        weightTotal += active[index]->weight;
    }

    const auto remaining = availableExtent - minimumTotal;
    int assignedExtra = 0;
    for (size_t index = 0; index < active.size(); ++index)
    {
        const auto extra = index + 1 == active.size()
            ? remaining - assignedExtra
            : static_cast<int>((static_cast<long long>(remaining) * active[index]->weight)
                               / std::max<long long>(1, weightTotal));
        extents[index] += extra;
        assignedExtra += extra;
    }

    return extents;
}

void WorkspaceModuleLayout::setActiveExtents(const std::vector<int>& extents)
{
    std::vector<Entry*> active;
    for (auto* entry : entriesInOrder())
        if (entry->available && entry->enabled && entry->placed && !entryIsFloating(*entry))
            active.push_back(entry);

    if (active.size() != extents.size())
        return;

    for (size_t index = 0; index < active.size(); ++index)
    {
        const auto extra = extents[index] - active[index]->descriptor.minimumExtent;
        active[index]->weight = std::max(1, extra);
    }
}

const std::vector<int>& WorkspaceModuleLayout::getColumnTrackWeights() const noexcept
{
    return columnTrackWeights;
}

const std::vector<int>& WorkspaceModuleLayout::getRowTrackWeights() const noexcept
{
    return rowTrackWeights;
}

WorkspaceGridMutationResult WorkspaceModuleLayout::setGridTrackWeights(
    std::vector<int> nextColumnWeights,
    std::vector<int> nextRowWeights)
{
    const auto valid = [] (const std::vector<int>& values, std::size_t maximumCount)
    {
        return values.size() <= maximumCount
            && std::all_of(values.begin(), values.end(), [] (int value)
            {
                return value >= 1 && value <= 1000000;
            });
    };
    if (!valid(nextColumnWeights, static_cast<std::size_t>(gridColumns))
        || !valid(nextRowWeights, maximumRowTracks))
    {
        return WorkspaceGridMutationResult::invalidPlacement;
    }
    if (nextColumnWeights == columnTrackWeights && nextRowWeights == rowTrackWeights)
        return WorkspaceGridMutationResult::noChange;
    const auto before = captureUserState();
    columnTrackWeights = std::move(nextColumnWeights);
    rowTrackWeights = std::move(nextRowWeights);
    return commitMutation(before);
}

std::string WorkspaceModuleLayout::serialiseUserState() const
{
    // The live grid deliberately ignores unavailable Mags so ordinary audio can
    // reclaim every pixel.  That means an unavailable Mag's remembered slot may
    // be occupied by a currently visible tile.  Persist a deterministic free
    // reservation for those dormant tiles; otherwise the next launch repeatedly
    // detects and repairs the same legacy overlap even though the visible layout
    // is already valid.
    struct ReservedTile
    {
        WorkspaceGridPlacement placement;
    };
    std::vector<ReservedTile> reservedTiles;
    for (const auto& item : gridItems())
        reservedTiles.push_back({ item.placement });

    std::unordered_map<std::string, WorkspaceGridPlacement> persistencePlacements;
    const auto tileIsFloating = [this] (const std::string& id)
    {
        return std::any_of(floating.begin(), floating.end(), [&id] (const FloatingTile& tile)
        {
            return tile.id == id;
        });
    };
    const auto reservePlacement = [&] (const std::string& id,
                                       WorkspaceGridPlacement origin)
    {
        const auto overlapsReserved = [&reservedTiles] (const WorkspaceGridPlacement& placement)
        {
            return std::any_of(reservedTiles.begin(), reservedTiles.end(),
                               [&placement] (const ReservedTile& tile)
            {
                return placement.overlaps(tile.placement);
            });
        };

        auto placement = origin;
        if (overlapsReserved(placement))
        {
            auto found = false;
            const auto searchRows = std::max(32, static_cast<int>(reservedTiles.size()) * 8 + 8);
            for (auto row = 0; row < searchRows && !found; ++row)
            {
                WorkspaceGridPlacement candidate { origin.column, row, origin.span };
                if (candidate.right() <= gridColumns && !overlapsReserved(candidate))
                {
                    placement = candidate;
                    found = true;
                }
            }
            for (auto row = 0; row < searchRows && !found; ++row)
                for (auto column = 0; column <= gridColumns - origin.span.columns && !found; ++column)
                {
                    WorkspaceGridPlacement candidate { column, row, origin.span };
                    if (!overlapsReserved(candidate))
                    {
                        placement = candidate;
                        found = true;
                    }
                }
        }
        persistencePlacements[id] = placement;
        reservedTiles.push_back({ placement });
    };

    // Opaque modules have no runtime availability signal, so preserve their
    // placement before reserving known-but-dormant Mags around them.
    for (const auto& module : inert)
        if (module.enabled && module.placed && module.tabStackId.empty()
            && !tileIsFloating(module.instanceId))
            reservePlacement(module.instanceId, module.placement);

    for (const auto& stack : stacks)
    {
        if (tileIsFloating(stack.id))
            continue;
        const auto hasActiveKnownTab = std::any_of(entries.begin(), entries.end(),
                                                   [&stack] (const Entry& entry)
        {
            return entry.tabStackId == stack.id && entry.available
                && entry.enabled && entry.placed;
        });
        // gridItems() already reserved stacks that have a live known tab.  A
        // dormant or opaque-only stack has no projected grid item, so it still
        // needs an explicit persistence reservation here.
        if (!hasActiveKnownTab)
            reservePlacement(stack.id, stack.placement);
    }

    for (const auto* entry : entriesInOrder())
        if (!entry->available && entry->enabled && entry->placed
            && entry->tabStackId.empty() && !entryIsFloating(*entry))
            reservePlacement(entry->descriptor.instanceId, entry->placement);

    const auto placementForPersistence = [&persistencePlacements]
        (const std::string& tileId, WorkspaceGridPlacement fallback)
    {
        if (const auto found = persistencePlacements.find(tileId);
            found != persistencePlacements.end())
            return found->second;
        return fallback;
    };

    auto root = parseJsonObjectOrNew(opaqueTopLevelJson);
    auto* rootObject = root.getDynamicObject();
    rootObject->setProperty("schema", workspacePresetSchema);
    rootObject->setProperty("version", currentWorkspacePresetVersion);
    rootObject->setProperty("presetId", juce::String::fromUTF8(presetId.c_str()));

    auto core = rootObject->getProperty("core");
    if (core.getDynamicObject() == nullptr)
        core = makeObject();
    core.getDynamicObject()->setProperty("instanceId", "core-waveform");
    core.getDynamicObject()->setProperty("placement", placementToVar(corePlacement));
    rootObject->setProperty("core", core);

    if (columnTrackWeights.empty() && rowTrackWeights.empty())
    {
        rootObject->removeProperty("gridTracks");
    }
    else
    {
        auto tracks = makeObject();
        juce::Array<juce::var> columns;
        for (const auto weight : columnTrackWeights)
            columns.add(weight);
        juce::Array<juce::var> rows;
        for (const auto weight : rowTrackWeights)
            rows.add(weight);
        tracks.getDynamicObject()->setProperty("columns", juce::var(columns));
        tracks.getDynamicObject()->setProperty("rows", juce::var(rows));
        rootObject->setProperty("gridTracks", tracks);
    }

    juce::Array<juce::var> moduleValues;
    for (const auto* entry : entriesInOrder())
    {
        const auto quarantined = std::any_of(inert.begin(), inert.end(), [entry] (const InertModule& module)
        {
            return module.quarantined && module.instanceId == entry->descriptor.instanceId;
        });
        if (quarantined)
            continue;
        auto module = parseJsonObjectOrNew(entry->opaqueRecordJson);
        auto* object = module.getDynamicObject();
        object->setProperty("instanceId", juce::String::fromUTF8(entry->descriptor.instanceId.c_str()));
        object->setProperty("moduleId", juce::String::fromUTF8(entry->descriptor.moduleTypeId.c_str()));
        object->setProperty("enabled", entry->enabled);
        object->setProperty("placed", entry->placed);
        object->setProperty("order", entry->order);
        object->setProperty("weight", entry->weight);
        const auto tileId = entry->tabStackId.empty()
            ? entry->descriptor.instanceId : entry->tabStackId;
        object->setProperty("placement",
                            placementToVar(placementForPersistence(tileId, entry->placement)));
        if (entry->tabStackId.empty())
            object->removeProperty("stackId");
        else
            object->setProperty("stackId", juce::String::fromUTF8(entry->tabStackId.c_str()));

        auto viewState = object->getProperty("viewState");
        if (viewState.getDynamicObject() == nullptr)
            viewState = makeObject();
        viewState.getDynamicObject()->setProperty("version", entry->viewStateVersion);
        auto payload = parseJsonObjectOrNew(entry->viewStateJson);
        viewState.getDynamicObject()->setProperty("payload", payload);
        object->setProperty("viewState", viewState);
        moduleValues.add(module);
    }
    for (const auto& module : inert)
    {
        auto value = parseJsonObjectOrNew(module.opaqueRecordJson);
        auto* object = value.getDynamicObject();
        object->setProperty("instanceId", juce::String::fromUTF8(module.instanceId.c_str()));
        object->setProperty("moduleId", juce::String::fromUTF8(module.moduleTypeId.c_str()));
        object->setProperty("enabled", module.enabled);
        object->setProperty("placed", module.placed);
        const auto tileId = module.tabStackId.empty() ? module.instanceId : module.tabStackId;
        object->setProperty("placement",
                            placementToVar(placementForPersistence(tileId, module.placement)));
        if (module.tabStackId.empty())
            object->removeProperty("stackId");
        else
            object->setProperty("stackId", juce::String::fromUTF8(module.tabStackId.c_str()));
        if (module.quarantined)
            object->setProperty("quarantined", true);
        moduleValues.add(value);
    }
    rootObject->setProperty("modules", juce::var(moduleValues));

    juce::Array<juce::var> stackValues;
    for (const auto& stack : stacks)
    {
        auto value = parseJsonObjectOrNew(stack.opaqueRecordJson);
        auto* object = value.getDynamicObject();
        object->setProperty("id", juce::String::fromUTF8(stack.id.c_str()));
        object->setProperty("placement",
                            placementToVar(placementForPersistence(stack.id, stack.placement)));
        juce::Array<juce::var> tabs;
        for (const auto& instanceId : stack.tabInstanceIds)
            tabs.add(juce::String::fromUTF8(instanceId.c_str()));
        object->setProperty("tabs", juce::var(tabs));
        object->setProperty("active", juce::String::fromUTF8(stack.activeInstanceId.c_str()));
        stackValues.add(value);
    }
    rootObject->setProperty("stacks", juce::var(stackValues));

    juce::Array<juce::var> floatingValues;
    for (const auto& tile : floating)
    {
        auto value = parseJsonObjectOrNew(tile.opaqueRecordJson);
        auto* object = value.getDynamicObject();
        object->setProperty("id", juce::String::fromUTF8(tile.id.c_str()));
        object->setProperty("privilegedCore", tile.privilegedCore);
        writeFloatingPlacement(*object, tile.placement);
        floatingValues.add(value);
    }
    for (const auto& rawRecord : opaqueFloatingRecords)
        floatingValues.add(parseJsonObjectOrNew(rawRecord));
    rootObject->setProperty("floating", juce::var(floatingValues));
    return compactJson(root);
}

WorkspaceRestoreResult WorkspaceModuleLayout::restoreUserState(const std::string& state)
{
    if (state.empty())
        return { WorkspaceRestoreStatus::noState, 0 };

    if (state.rfind("v1", 0) == 0)
    {
        struct LegacyPlacementIntent
        {
            int order = 0;
            size_t tokenIndex = 0;
            std::optional<WorkspaceModuleId> knownId;
            std::string inertInstanceId;
        };

        const auto tokens = split(state, '|');
        if (tokens.empty() || tokens.front() != "v1")
            return { WorkspaceRestoreStatus::corrupt, 0 };

        auto candidate = entries;
        std::vector<InertModule> candidateInert;
        std::vector<LegacyPlacementIntent> placementIntents;
        std::unordered_set<std::string> restoredIds;
        for (size_t tokenIndex = 1; tokenIndex < tokens.size(); ++tokenIndex)
        {
            const auto fields = split(tokens[tokenIndex], ':');
            if (fields.size() != 4 || !restoredIds.insert(fields.front()).second)
                return { WorkspaceRestoreStatus::corrupt, 0 };

            int enabled = 0;
            int order = 0;
            int weight = 0;
            if (!parseInt(fields[1], enabled) || !parseInt(fields[2], order) || !parseInt(fields[3], weight)
                || (enabled != 0 && enabled != 1) || order < 0 || weight < 1)
            {
                return { WorkspaceRestoreStatus::corrupt, 0 };
            }

            const auto id = moduleIdForKey(fields.front());
            if (!id.has_value())
            {
                auto opaque = makeObject();
                opaque.getDynamicObject()->setProperty("instanceId", juce::String::fromUTF8(fields.front().c_str()));
                opaque.getDynamicObject()->setProperty("moduleId", juce::String::fromUTF8(fields.front().c_str()));
                opaque.getDynamicObject()->setProperty("enabled", enabled != 0);
                opaque.getDynamicObject()->setProperty("placed", enabled != 0);
                opaque.getDynamicObject()->setProperty("legacyOrder", order);
                opaque.getDynamicObject()->setProperty("legacyWeight", weight);
                const WorkspaceGridPlacement placement { 3, 1 + static_cast<int>(tokenIndex), { 1, 1 } };
                opaque.getDynamicObject()->setProperty("placement", placementToVar(placement));
                candidateInert.push_back({ fields.front(),
                                           fields.front(),
                                           placement,
                                           {},
                                           enabled != 0,
                                           enabled != 0,
                                           false,
                                           compactJson(opaque) });
                if (enabled != 0)
                    placementIntents.push_back({ order, tokenIndex, std::nullopt, fields.front() });
                continue;
            }

            const auto found = std::find_if(candidate.begin(), candidate.end(), [id] (const Entry& entry)
            {
                return entry.descriptor.id == *id;
            });
            if (found != candidate.end())
            {
                found->enabled = enabled != 0;
                found->placed = enabled != 0;
                found->order = order;
                found->weight = weight;
                found->tabStackId.clear();
                if (enabled != 0)
                    placementIntents.push_back({ order, tokenIndex, id, {} });
            }
        }

        for (auto& entry : candidate)
        {
            if (!entry.descriptor.restoreDefaultWhenAbsent
                || restoredIds.count(entry.descriptor.instanceId) != 0)
                continue;
            entry.enabled = true;
            entry.placed = entry.descriptor.defaultPlaced;
            entry.tabStackId.clear();
            if (entry.placed)
                placementIntents.push_back({ entry.descriptor.defaultOrder,
                    tokens.size() + placementIntents.size(), entry.descriptor.id, {} });
        }

        std::stable_sort(placementIntents.begin(), placementIntents.end(), [] (const auto& left, const auto& right)
        {
            return std::tie(left.order, left.tokenIndex)
                 < std::tie(right.order, right.tokenIndex);
        });
        auto row = 0;
        for (const auto& intent : placementIntents)
        {
            if (intent.knownId.has_value())
            {
                const auto found = std::find_if(candidate.begin(), candidate.end(), [&intent] (const Entry& entry)
                {
                    return entry.descriptor.id == *intent.knownId;
                });
                if (found == candidate.end())
                    continue;
                auto span = found->descriptor.preferredSpan;
                if (placementIntents.size() == 1)
                    span.rows = std::clamp(2,
                                           found->descriptor.minimumSpan.rows,
                                           found->descriptor.maximumSpan.rows);
                found->placement = { gridColumns - span.columns, row, span };
                row += span.rows;
                continue;
            }

            const auto found = std::find_if(candidateInert.begin(), candidateInert.end(), [&intent] (const InertModule& module)
            {
                return module.instanceId == intent.inertInstanceId;
            });
            if (found != candidateInert.end())
            {
                found->placement = { gridColumns - 1, row, { 1, 1 } };
                row += 1;
            }
        }

        entries = std::move(candidate);
        inert = std::move(candidateInert);
        stacks.clear();
        floating.clear();
        opaqueFloatingRecords.clear();
        columnTrackWeights.clear();
        rowTrackWeights.clear();
        corePlacement = { 0, 0, { 3, 2 } };
        nextStackSequence = 1;
        presetId = "migrated-v1";
        opaqueTopLevelJson = "{}";
        undoHistory.clear();
        redoHistory.clear();
        lastClosedDock.reset();
        return { WorkspaceRestoreStatus::migratedLegacyV1, 0 };
    }

    juce::var root;
    if (juce::JSON::parse(juce::String::fromUTF8(state.c_str()), root).failed())
        return { WorkspaceRestoreStatus::corrupt, 0 };
    const auto* rootObject = root.getDynamicObject();
    if (rootObject == nullptr || rootObject->getProperty("schema").toString() != workspacePresetSchema)
        return { WorkspaceRestoreStatus::corrupt, 0 };

    int version = 0;
    if (!readIntProperty(*rootObject, "version", version) || version < currentWorkspacePresetVersion)
        return { WorkspaceRestoreStatus::corrupt, 0 };
    if (version > currentWorkspacePresetVersion)
        return { WorkspaceRestoreStatus::futureSchema, 0 };

    const auto parsedPresetId = rootObject->getProperty("presetId").toString().toStdString();
    if (parsedPresetId.empty())
        return { WorkspaceRestoreStatus::corrupt, 0 };

    WorkspaceGridPlacement candidateCore { 0, 0, { 3, 2 } };
    if (const auto* core = rootObject->getProperty("core").getDynamicObject())
        if (const auto placement = placementFromVar(core->getProperty("placement")))
            candidateCore = *placement;
    if (candidateCore.span.columns < 2)
        candidateCore = { 0, 0, { 3, 2 } };

    std::vector<int> candidateColumnTrackWeights;
    std::vector<int> candidateRowTrackWeights;
    if (const auto* tracks = rootObject->getProperty("gridTracks").getDynamicObject())
    {
        const auto readWeights = [] (const juce::var& value,
                                     int maximumCount,
                                     std::vector<int>& destination)
        {
            const auto* values = value.getArray();
            if (values == nullptr || values->size() > maximumCount)
                return false;
            destination.clear();
            destination.reserve(static_cast<size_t>(values->size()));
            for (const auto& item : *values)
            {
                if ((!item.isInt() && !item.isInt64())
                    || static_cast<juce::int64>(item) < 1
                    || static_cast<juce::int64>(item) > 1000000)
                {
                    return false;
                }
                destination.push_back(static_cast<int>(static_cast<juce::int64>(item)));
            }
            return true;
        };
        if (!readWeights(tracks->getProperty("columns"), gridColumns,
                         candidateColumnTrackWeights)
            || !readWeights(tracks->getProperty("rows"), maximumRowTracks,
                            candidateRowTrackWeights))
        {
            return { WorkspaceRestoreStatus::corrupt, 0 };
        }
    }

    const auto* moduleArray = rootObject->getProperty("modules").getArray();
    const auto* stackArray = rootObject->getProperty("stacks").getArray();
    if (moduleArray == nullptr || stackArray == nullptr)
        return { WorkspaceRestoreStatus::corrupt, 0 };

    auto candidateEntries = entries;
    for (auto& entry : candidateEntries)
    {
        entry.enabled = false;
        entry.placed = false;
        entry.tabStackId.clear();
        entry.opaqueRecordJson = "{}";
    }
    std::vector<InertModule> candidateInert;
    std::unordered_set<std::string> instanceIds;
    int quarantinedCount = 0;

    for (const auto& moduleValue : *moduleArray)
    {
        const auto* object = moduleValue.getDynamicObject();
        if (object == nullptr)
            return { WorkspaceRestoreStatus::corrupt, 0 };
        const auto instanceId = object->getProperty("instanceId").toString().toStdString();
        const auto moduleTypeId = object->getProperty("moduleId").toString().toStdString();
        const auto enabledValue = object->getProperty("enabled");
        const auto placedValue = object->getProperty("placed");
        const auto placement = placementFromVar(object->getProperty("placement"));
        if (instanceId.empty() || moduleTypeId.empty() || !instanceIds.insert(instanceId).second
            || !enabledValue.isBool() || !placedValue.isBool() || !placement.has_value())
        {
            return { WorkspaceRestoreStatus::corrupt, 0 };
        }

        const auto stackId = object->getProperty("stackId").toString().toStdString();
        auto known = std::find_if(candidateEntries.begin(), candidateEntries.end(), [&instanceId] (const Entry& entry)
        {
            return entry.descriptor.instanceId == instanceId;
        });
        const auto rawRecord = compactJson(moduleValue);
        if (known == candidateEntries.end() || known->descriptor.moduleTypeId != moduleTypeId)
        {
            candidateInert.push_back({ instanceId,
                                       moduleTypeId,
                                       *placement,
                                       stackId,
                                       static_cast<bool>(enabledValue),
                                       static_cast<bool>(placedValue),
                                       known != candidateEntries.end(),
                                       rawRecord });
            if (known != candidateEntries.end())
                ++quarantinedCount;
            continue;
        }

        int order = 0;
        int weight = 0;
        if (!readIntProperty(*object, "order", order) || !readIntProperty(*object, "weight", weight)
            || order < 0 || weight < 1
            || placement->span.columns < known->descriptor.minimumSpan.columns
            || placement->span.rows < known->descriptor.minimumSpan.rows
            || placement->span.columns > known->descriptor.maximumSpan.columns
            || placement->span.rows > known->descriptor.maximumSpan.rows)
        {
            return { WorkspaceRestoreStatus::corrupt, 0 };
        }

        const auto* viewState = object->getProperty("viewState").getDynamicObject();
        int viewStateVersion = 0;
        const auto payload = viewState != nullptr ? viewState->getProperty("payload") : juce::var();
        if (viewState == nullptr || !readIntProperty(*viewState, "version", viewStateVersion)
            || viewStateVersion < 1 || !validateViewState(payload))
        {
            candidateInert.push_back({ instanceId,
                                       moduleTypeId,
                                       *placement,
                                       stackId,
                                       static_cast<bool>(enabledValue),
                                       static_cast<bool>(placedValue),
                                       true,
                                       rawRecord });
            ++quarantinedCount;
            continue;
        }

        known->enabled = static_cast<bool>(enabledValue);
        known->placed = static_cast<bool>(placedValue);
        known->order = order;
        known->weight = weight;
        known->placement = *placement;
        known->gridCustomized = true;
        known->tabStackId = stackId;
        known->viewStateVersion = viewStateVersion;
        known->viewStateJson = compactJson(payload);
        known->opaqueRecordJson = rawRecord;
    }

    std::vector<TabStack> candidateStacks;
    std::unordered_set<std::string> stackIds;
    std::unordered_set<std::string> stackedInstances;
    int candidateNextStackSequence = 1;
    for (const auto& stackValue : *stackArray)
    {
        const auto* object = stackValue.getDynamicObject();
        const auto* tabs = object != nullptr ? object->getProperty("tabs").getArray() : nullptr;
        const auto placement = object != nullptr
            ? placementFromVar(object->getProperty("placement"))
            : std::nullopt;
        if (object == nullptr || tabs == nullptr || tabs->size() < 2 || !placement.has_value())
            return { WorkspaceRestoreStatus::corrupt, 0 };
        const auto id = object->getProperty("id").toString().toStdString();
        const auto active = object->getProperty("active").toString().toStdString();
        if (id.empty() || active.empty() || !stackIds.insert(id).second)
            return { WorkspaceRestoreStatus::corrupt, 0 };

        std::vector<std::string> tabIds;
        for (const auto& tab : *tabs)
        {
            if (!tab.isString())
                return { WorkspaceRestoreStatus::corrupt, 0 };
            const auto instanceId = tab.toString().toStdString();
            if (instanceIds.find(instanceId) == instanceIds.end()
                || !stackedInstances.insert(instanceId).second)
            {
                return { WorkspaceRestoreStatus::corrupt, 0 };
            }
            const auto known = std::find_if(candidateEntries.begin(), candidateEntries.end(), [&instanceId, &id] (const Entry& entry)
            {
                return entry.descriptor.instanceId == instanceId && entry.placed
                    && entry.tabStackId == id;
            });
            const auto opaque = std::find_if(candidateInert.begin(), candidateInert.end(), [&instanceId, &id] (const InertModule& module)
            {
                return module.instanceId == instanceId && module.placed
                    && module.tabStackId == id;
            });
            if (known == candidateEntries.end() && opaque == candidateInert.end())
                return { WorkspaceRestoreStatus::corrupt, 0 };
            tabIds.push_back(instanceId);
        }
        if (std::find(tabIds.begin(), tabIds.end(), active) == tabIds.end())
            return { WorkspaceRestoreStatus::corrupt, 0 };

        candidateStacks.push_back({ id, *placement, tabIds, active, compactJson(stackValue) });
        if (id.rfind("stack-", 0) == 0)
        {
            int sequence = 0;
            if (parseInt(id.substr(6), sequence))
                candidateNextStackSequence = std::max(candidateNextStackSequence, sequence + 1);
        }
    }

    for (auto& entry : candidateEntries)
    {
        if (entry.tabStackId.empty())
            continue;
        const auto found = std::find_if(candidateStacks.begin(), candidateStacks.end(), [&entry] (const TabStack& stack)
        {
            return stack.id == entry.tabStackId
                && std::find(stack.tabInstanceIds.begin(), stack.tabInstanceIds.end(), entry.descriptor.instanceId)
                       != stack.tabInstanceIds.end();
        });
        if (found == candidateStacks.end())
            return { WorkspaceRestoreStatus::corrupt, 0 };
        entry.placement = found->placement;
    }
    for (auto& module : candidateInert)
    {
        if (module.tabStackId.empty())
            continue;
        const auto found = std::find_if(candidateStacks.begin(), candidateStacks.end(), [&module] (const TabStack& stack)
        {
            return stack.id == module.tabStackId
                && std::find(stack.tabInstanceIds.begin(), stack.tabInstanceIds.end(), module.instanceId)
                       != stack.tabInstanceIds.end();
        });
        if (found == candidateStacks.end())
            return { WorkspaceRestoreStatus::corrupt, 0 };
        module.placement = found->placement;
    }

    std::vector<FloatingTile> candidateFloating;
    std::vector<std::string> candidateOpaqueFloatingRecords;
    std::unordered_set<std::string> floatingIds;
    if (const auto floatingValue = rootObject->getProperty("floating"); !floatingValue.isVoid())
    {
        const auto* floatingArray = floatingValue.getArray();
        if (floatingArray == nullptr)
            return { WorkspaceRestoreStatus::corrupt, 0 };

        const auto isKnownTileId = [&] (const std::string& id)
        {
            if (id == "core-waveform")
                return true;
            if (std::any_of(candidateStacks.begin(), candidateStacks.end(), [&] (const TabStack& stack)
                {
                    return stack.id == id;
                }))
            {
                return true;
            }
            return std::any_of(candidateEntries.begin(), candidateEntries.end(), [&] (const Entry& entry)
            {
                return entry.descriptor.instanceId == id;
            }) || std::any_of(candidateInert.begin(), candidateInert.end(), [&] (const InertModule& module)
            {
                return module.instanceId == id;
            });
        };

        const auto isEligibleTileId = [&] (const std::string& id)
        {
            if (id == "core-waveform")
                return true;
            if (std::any_of(candidateStacks.begin(), candidateStacks.end(), [&] (const TabStack& stack)
                {
                    return stack.id == id;
                }))
            {
                return true;
            }
            return std::any_of(candidateEntries.begin(), candidateEntries.end(), [&] (const Entry& entry)
            {
                return entry.descriptor.instanceId == id && entry.enabled && entry.placed
                    && entry.tabStackId.empty();
            }) || std::any_of(candidateInert.begin(), candidateInert.end(), [&] (const InertModule& module)
            {
                return module.instanceId == id && module.enabled && module.placed
                    && module.tabStackId.empty();
            });
        };

        for (const auto& record : *floatingArray)
        {
            const auto rawRecord = compactJson(record);
            const auto* object = record.getDynamicObject();
            if (object == nullptr)
            {
                candidateOpaqueFloatingRecords.push_back(rawRecord);
                continue;
            }
            const auto id = object->getProperty("id").toString().toStdString();
            const auto privilegedValue = object->getProperty("privilegedCore");
            if (id.empty() || !isKnownTileId(id))
            {
                candidateOpaqueFloatingRecords.push_back(rawRecord);
                continue;
            }
            const auto placement = floatingPlacementFromVar(*object);
            const auto privilegedCore = id == "core-waveform";
            if (!isEligibleTileId(id) || !floatingIds.insert(id).second
                || !privilegedValue.isBool()
                || static_cast<bool>(privilegedValue) != privilegedCore
                || !placement.has_value())
            {
                ++quarantinedCount;
                continue;
            }
            candidateFloating.push_back({ id, *placement, privilegedCore, rawRecord });
        }
    }

    struct Tile
    {
        std::string id;
        WorkspaceGridPlacement placement;
    };
    const auto tileIsFloating = [&candidateFloating] (const std::string& id)
    {
        return std::any_of(candidateFloating.begin(), candidateFloating.end(), [&id] (const FloatingTile& tile)
        {
            return tile.id == id;
        });
    };
    std::vector<Tile> tiles { { "core-waveform", candidateCore } };
    for (const auto& stack : candidateStacks)
        if (!tileIsFloating(stack.id))
            tiles.push_back({ stack.id, stack.placement });
    for (const auto& entry : candidateEntries)
        if (entry.enabled && entry.placed && entry.tabStackId.empty()
            && !tileIsFloating(entry.descriptor.instanceId))
            tiles.push_back({ entry.descriptor.instanceId, entry.placement });
    for (const auto& module : candidateInert)
        if (module.enabled && module.placed && module.tabStackId.empty()
            && !tileIsFloating(module.instanceId))
            tiles.push_back({ module.instanceId, module.placement });
    const auto updateCandidateTilePlacement = [&] (const std::string& id,
                                                    WorkspaceGridPlacement placement)
    {
        if (const auto stack = std::find_if(candidateStacks.begin(), candidateStacks.end(),
                                            [&id] (const TabStack& item) { return item.id == id; });
            stack != candidateStacks.end())
        {
            stack->placement = placement;
            for (auto& entry : candidateEntries)
                if (entry.tabStackId == id)
                    entry.placement = placement;
            for (auto& module : candidateInert)
                if (module.tabStackId == id)
                    module.placement = placement;
            return true;
        }
        if (const auto entry = std::find_if(candidateEntries.begin(), candidateEntries.end(),
                                            [&id] (const Entry& item)
        {
            return item.descriptor.instanceId == id;
        }); entry != candidateEntries.end())
        {
            entry->placement = placement;
            return true;
        }
        if (const auto module = std::find_if(candidateInert.begin(), candidateInert.end(),
                                             [&id] (const InertModule& item)
        {
            return item.instanceId == id;
        }); module != candidateInert.end())
        {
            module->placement = placement;
            return true;
        }
        return false;
    };

    // Early V2 builds serialised several right-rail Mags into the same grid
    // cell because their visual projection, not their structural placement,
    // separated them.  The new split model must migrate that exact legacy
    // overlap instead of trapping the user forever in a read-only safe layout.
    // Core overlap and malformed records still fail closed; only later non-core
    // tiles are packed into the nearest free rectangle.
    std::vector<Tile> acceptedTiles;
    acceptedTiles.reserve(tiles.size());
    int repairedTileCount = 0;
    for (auto& tile : tiles)
    {
        if (tile.id == "core-waveform")
        {
            acceptedTiles.push_back(tile);
            continue;
        }
        if (tile.placement.overlaps(candidateCore))
            return { WorkspaceRestoreStatus::corrupt, 0 };

        const auto overlapsAccepted = [&acceptedTiles] (const WorkspaceGridPlacement& placement)
        {
            return std::any_of(acceptedTiles.begin(), acceptedTiles.end(), [&placement] (const Tile& item)
            {
                return placement.overlaps(item.placement);
            });
        };
        if (overlapsAccepted(tile.placement))
        {
            auto replacement = tile.placement;
            auto found = false;
            const auto searchRows = std::max(32,
                static_cast<int>(tiles.size() * 8 + acceptedTiles.size() * 4));

            // Preserve the old semantic rail first: move downward in the same
            // column before considering a different column.
            for (auto row = 0; row < searchRows && !found; ++row)
            {
                replacement = { tile.placement.column, row, tile.placement.span };
                found = replacement.right() <= gridColumns && !overlapsAccepted(replacement);
            }
            for (auto row = 0; row < searchRows && !found; ++row)
                for (auto column = 0;
                     column <= gridColumns - tile.placement.span.columns && !found;
                     ++column)
                {
                    replacement = { column, row, tile.placement.span };
                    found = !overlapsAccepted(replacement);
                }
            if (!found || !updateCandidateTilePlacement(tile.id, replacement))
                return { WorkspaceRestoreStatus::corrupt, 0 };
            tile.placement = replacement;
            ++repairedTileCount;
        }
        acceptedTiles.push_back(tile);
    }
    if (repairedTileCount > 0)
    {
        candidateColumnTrackWeights.clear();
        candidateRowTrackWeights.clear();
    }

    // Validate the original preset first. Only truly absent opt-in sidebars
    // get migrated; a hidden, mismatched or quarantined record is not absent.
    // Pack new entries around all preserved tiles, including unavailable ones.
    for (auto& entry : candidateEntries)
    {
        if (!entry.descriptor.restoreDefaultWhenAbsent
            || instanceIds.count(entry.descriptor.instanceId) != 0)
            continue;
        entry.enabled = true;
        entry.placed = entry.descriptor.defaultPlaced;
        entry.gridCustomized = true;
        entry.placement.span = entry.descriptor.preferredSpan;
        if (!entry.placed)
            continue;
        auto lastRow = 0;
        for (const auto& tile : tiles)
            lastRow = std::max(lastRow, tile.placement.bottom());
        bool placed = false;
        for (int row = 0; row <= lastRow && !placed; ++row)
        {
            for (int column = 0; column <= gridColumns - entry.placement.span.columns; ++column)
            {
                const WorkspaceGridPlacement candidate { column, row, entry.placement.span };
                if (std::any_of(tiles.begin(), tiles.end(), [&] (const Tile& tile)
                    { return candidate.overlaps(tile.placement); }))
                    continue;
                entry.placement = candidate;
                tiles.push_back({ entry.descriptor.instanceId, candidate });
                placed = true;
                break;
            }
        }
    }

    entries = std::move(candidateEntries);
    stacks = std::move(candidateStacks);
    inert = std::move(candidateInert);
    floating = std::move(candidateFloating);
    opaqueFloatingRecords = std::move(candidateOpaqueFloatingRecords);
    corePlacement = candidateCore;
    columnTrackWeights = std::move(candidateColumnTrackWeights);
    rowTrackWeights = std::move(candidateRowTrackWeights);
    nextStackSequence = candidateNextStackSequence;
    presetId = parsedPresetId;
    opaqueTopLevelJson = compactJson(root);
    undoHistory.clear();
    redoHistory.clear();
    lastClosedDock.reset();
    return { repairedTileCount > 0 ? WorkspaceRestoreStatus::repairedLegacyOverlap
                                   : WorkspaceRestoreStatus::applied,
             quarantinedCount,
             repairedTileCount };
}

void WorkspaceModuleLayout::resetUserState()
{
    for (auto& entry : entries)
    {
        entry.enabled = true;
        entry.placed = entry.descriptor.defaultPlaced;
        entry.order = entry.descriptor.defaultOrder;
        entry.weight = entry.descriptor.defaultWeight;
        entry.placement = { entry.descriptor.defaultGridColumn < 0
                                ? gridColumns - entry.descriptor.preferredSpan.columns
                                : std::clamp(entry.descriptor.defaultGridColumn,
                                             0,
                                             gridColumns - entry.descriptor.preferredSpan.columns),
                            std::max(0, entry.descriptor.defaultGridRow),
                            entry.descriptor.preferredSpan };
        entry.gridCustomized = false;
        entry.tabStackId.clear();
        entry.viewStateVersion = 1;
        entry.viewStateJson = "{}";
        entry.opaqueRecordJson = "{}";
    }
    corePlacement = { 0, 0, { 3, 2 } };
    stacks.clear();
    inert.clear();
    floating.clear();
    opaqueFloatingRecords.clear();
    columnTrackWeights.clear();
    rowTrackWeights.clear();
    nextStackSequence = 1;
    presetId = "current";
    opaqueTopLevelJson = "{}";
    ensureActivePlacements();
    reflowDefaultPlacements();
    fillAllGridHoles();
    undoHistory.clear();
    redoHistory.clear();
    lastClosedDock.reset();
}

WorkspaceModuleLayout::UserState WorkspaceModuleLayout::captureUserState() const
{
    return { corePlacement,
             entries,
             stacks,
             inert,
             floating,
             opaqueFloatingRecords,
             columnTrackWeights,
             rowTrackWeights,
             nextStackSequence };
}

WorkspaceModuleLayout::TrackWeightSnapshot
WorkspaceModuleLayout::captureTrackWeightSnapshot() const
{
    const auto collectTrackStarts = [] (const std::vector<WorkspaceGridItem>& items,
                                        bool columns)
    {
        std::vector<int> boundaries;
        boundaries.reserve(items.size() * 2);
        for (const auto& item : items)
        {
            boundaries.push_back(columns ? item.placement.column : item.placement.row);
            boundaries.push_back(columns ? item.placement.right() : item.placement.bottom());
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                         boundaries.end());
        std::vector<int> starts;
        starts.reserve(boundaries.size());
        for (std::size_t index = 0; index + 1 < boundaries.size(); ++index)
            if (boundaries[index + 1] > boundaries[index])
                starts.push_back(boundaries[index]);
        return starts;
    };

    const auto items = gridItems();
    const auto columnTrackStarts = collectTrackStarts(items, true);
    const auto rowTrackStarts = collectTrackStarts(items, false);
    return { columnTrackStarts.size(),
             rowTrackStarts.size(),
             columnTrackWeights,
             rowTrackWeights,
             columnTrackStarts,
             rowTrackStarts };
}

void WorkspaceModuleLayout::preserveUnchangedTrackWeights(
    const TrackWeightSnapshot& snapshot)
{
    const auto current = captureTrackWeightSnapshot();
    if (snapshot.columnTrackCount == current.columnTrackCount
        && snapshot.columnWeights.size() == snapshot.columnTrackCount)
    {
        columnTrackWeights = snapshot.columnWeights;
    }
    else
    {
        columnTrackWeights.clear();
    }

    if (snapshot.rowTrackCount == current.rowTrackCount
        && snapshot.rowWeights.size() == snapshot.rowTrackCount)
    {
        rowTrackWeights = snapshot.rowWeights;
    }
    else
    {
        rowTrackWeights.clear();
    }
}

void WorkspaceModuleLayout::preserveTrackWeightsAfterSplit(
    const TrackWeightSnapshot& snapshot,
    WorkspaceGridPlacement groupRegion,
    WorkspaceDockZone zone)
{
    preserveUnchangedTrackWeights(snapshot);

    const auto horizontal = zone == WorkspaceDockZone::left
                         || zone == WorkspaceDockZone::right;
    const auto vertical = zone == WorkspaceDockZone::top
                       || zone == WorkspaceDockZone::bottom;
    if (!horizontal && !vertical)
        return;

    const auto& oldStarts = horizontal ? snapshot.columnTrackStarts
                                       : snapshot.rowTrackStarts;
    const auto& oldWeights = horizontal ? snapshot.columnWeights
                                        : snapshot.rowWeights;
    auto& nextWeights = horizontal ? columnTrackWeights : rowTrackWeights;
    const auto currentTrackCount = horizontal
        ? captureTrackWeightSnapshot().columnTrackCount
        : captureTrackWeightSnapshot().rowTrackCount;
    if (oldWeights.size() != oldStarts.size()
        || currentTrackCount != oldStarts.size() + 1)
    {
        return;
    }

    std::vector<std::size_t> groupIndexes;
    const auto groupStart = horizontal ? groupRegion.column : groupRegion.row;
    const auto groupEnd = horizontal ? groupRegion.right() : groupRegion.bottom();
    for (std::size_t index = 0; index < oldStarts.size(); ++index)
        if (oldStarts[index] >= groupStart && oldStarts[index] < groupEnd)
            groupIndexes.push_back(index);
    if (groupIndexes.empty())
        return;

    const auto physicalFloor = horizontal ? 72 : 48;
    auto groupTotal = 0;
    for (const auto index : groupIndexes)
        groupTotal += oldWeights[index];
    const auto minimumGroupTotal = physicalFloor
                                 * static_cast<int>(groupIndexes.size() + 1);
    if (groupTotal < minimumGroupTotal)
        return;

    const auto maximumNewExtent = groupTotal
                                - physicalFloor * static_cast<int>(groupIndexes.size());
    const auto newExtent = std::clamp(groupTotal / 2,
                                      physicalFloor,
                                      maximumNewExtent);
    const auto remainingGroupExtent = groupTotal - newExtent;
    const auto remainingFlexible = remainingGroupExtent
                                 - physicalFloor * static_cast<int>(groupIndexes.size());
    auto oldFlexibleTotal = 0LL;
    for (const auto index : groupIndexes)
        oldFlexibleTotal += std::max(0, oldWeights[index] - physicalFloor);

    std::vector<int> resizedGroup(groupIndexes.size(), physicalFloor);
    auto assignedFlexible = 0;
    for (std::size_t offset = 0; offset < groupIndexes.size(); ++offset)
    {
        const auto extra = offset + 1 == groupIndexes.size()
            ? remainingFlexible - assignedFlexible
            : static_cast<int>((static_cast<long long>(remainingFlexible)
                                * std::max(0, oldWeights[groupIndexes[offset]] - physicalFloor))
                               / std::max(1LL, oldFlexibleTotal));
        resizedGroup[offset] += extra;
        assignedFlexible += extra;
    }

    const auto insertBefore = zone == WorkspaceDockZone::left
                           || zone == WorkspaceDockZone::top;
    const auto firstGroupIndex = groupIndexes.front();
    const auto lastGroupIndex = groupIndexes.back();
    std::vector<int> transformed;
    transformed.reserve(oldWeights.size() + 1);
    auto groupOffset = std::size_t { 0 };
    for (std::size_t index = 0; index < oldWeights.size(); ++index)
    {
        if (insertBefore && index == firstGroupIndex)
            transformed.push_back(newExtent);
        if (groupOffset < groupIndexes.size() && index == groupIndexes[groupOffset])
            transformed.push_back(resizedGroup[groupOffset++]);
        else
            transformed.push_back(oldWeights[index]);
        if (!insertBefore && index == lastGroupIndex)
            transformed.push_back(newExtent);
    }
    if (transformed.size() == oldWeights.size() + 1)
        nextWeights = std::move(transformed);
}

void WorkspaceModuleLayout::restoreUserState(const UserState& state)
{
    corePlacement = state.corePlacement;
    stacks = state.stacks;
    inert = state.inert;
    floating = state.floating;
    opaqueFloatingRecords = state.opaqueFloatingRecords;
    columnTrackWeights = state.columnTrackWeights;
    rowTrackWeights = state.rowTrackWeights;
    nextStackSequence = state.nextStackSequence;
    for (auto& entry : entries)
    {
        const auto found = std::find_if(state.entries.begin(), state.entries.end(), [&entry] (const Entry& saved)
        {
            return saved.descriptor.id == entry.descriptor.id;
        });
        if (found == state.entries.end())
            continue;
        entry.enabled = found->enabled;
        entry.placed = found->placed;
        entry.order = found->order;
        entry.weight = found->weight;
        entry.placement = found->placement;
        entry.gridCustomized = found->gridCustomized;
        entry.tabStackId = found->tabStackId;
        entry.viewStateVersion = found->viewStateVersion;
        entry.viewStateJson = found->viewStateJson;
        entry.opaqueRecordJson = found->opaqueRecordJson;
    }
}

bool WorkspaceModuleLayout::userStateEquals(const UserState& left, const UserState& right) const
{
    if (!(left.corePlacement == right.corePlacement)
        || left.entries.size() != right.entries.size()
        || left.stacks.size() != right.stacks.size()
        || left.inert.size() != right.inert.size()
        || left.floating.size() != right.floating.size()
        || left.opaqueFloatingRecords != right.opaqueFloatingRecords
        || left.columnTrackWeights != right.columnTrackWeights
        || left.rowTrackWeights != right.rowTrackWeights
        || left.nextStackSequence != right.nextStackSequence)
        return false;
    for (const auto& leftEntry : left.entries)
    {
        const auto found = std::find_if(right.entries.begin(), right.entries.end(), [&leftEntry] (const Entry& entry)
        {
            return entry.descriptor.id == leftEntry.descriptor.id;
        });
        if (found == right.entries.end()
            || found->enabled != leftEntry.enabled
            || found->placed != leftEntry.placed
            || found->order != leftEntry.order
            || found->weight != leftEntry.weight
            || !(found->placement == leftEntry.placement)
            || found->gridCustomized != leftEntry.gridCustomized
            || found->tabStackId != leftEntry.tabStackId
            || found->viewStateVersion != leftEntry.viewStateVersion
            || found->viewStateJson != leftEntry.viewStateJson
            || found->opaqueRecordJson != leftEntry.opaqueRecordJson)
        {
            return false;
        }
    }
    for (size_t index = 0; index < left.stacks.size(); ++index)
    {
        const auto& a = left.stacks[index];
        const auto& b = right.stacks[index];
        if (a.id != b.id || !(a.placement == b.placement)
            || a.tabInstanceIds != b.tabInstanceIds
            || a.activeInstanceId != b.activeInstanceId
            || a.opaqueRecordJson != b.opaqueRecordJson)
            return false;
    }
    for (size_t index = 0; index < left.inert.size(); ++index)
    {
        const auto& a = left.inert[index];
        const auto& b = right.inert[index];
        if (a.instanceId != b.instanceId || a.moduleTypeId != b.moduleTypeId
            || !(a.placement == b.placement) || a.tabStackId != b.tabStackId
            || a.enabled != b.enabled || a.placed != b.placed
            || a.quarantined != b.quarantined || a.opaqueRecordJson != b.opaqueRecordJson)
        {
            return false;
        }
    }
    for (size_t index = 0; index < left.floating.size(); ++index)
    {
        const auto& a = left.floating[index];
        const auto& b = right.floating[index];
        if (a.id != b.id || !(a.placement == b.placement)
            || a.privilegedCore != b.privilegedCore
            || a.opaqueRecordJson != b.opaqueRecordJson)
        {
            return false;
        }
    }
    return true;
}

WorkspaceGridPlacement WorkspaceModuleLayout::normalisePlacement(const Entry& entry,
                                                                 WorkspaceGridPlacement placement) const
{
    placement.span.columns = std::clamp(placement.span.columns,
                                        entry.descriptor.minimumSpan.columns,
                                        entry.descriptor.maximumSpan.columns);
    placement.span.rows = std::clamp(placement.span.rows,
                                     entry.descriptor.minimumSpan.rows,
                                     entry.descriptor.maximumSpan.rows);
    placement.column = std::clamp(placement.column, 0, gridColumns - placement.span.columns);
    placement.row = std::max(0, placement.row);
    return placement;
}

bool WorkspaceModuleLayout::placementIsFree(WorkspaceModuleId movingId,
                                            const WorkspaceGridPlacement& placement) const
{
    if (placement.column < 0 || placement.row < 0 || placement.right() > gridColumns
        || placement.span.columns < 1 || placement.span.rows < 1)
    {
        return false;
    }
    if (movingId != WorkspaceModuleId::coreWaveform && placement.overlaps(corePlacement))
        return false;

    for (const auto& entry : entries)
    {
        if (!entry.available || !entry.enabled || !entry.placed || entryIsFloating(entry)
            || entry.descriptor.id == movingId)
            continue;
        if (const auto* moving = findEntry(movingId); moving != nullptr && entriesShareTile(*moving, entry))
            continue;
        if (placement.overlaps(entry.placement))
            return false;
    }
    for (const auto& module : inert)
        if (module.enabled && module.placed && placement.overlaps(module.placement))
            return false;
    return true;
}

std::optional<WorkspaceGridPlacement> WorkspaceModuleLayout::nearestFreePlacement(
    const Entry& entry,
    WorkspaceGridPlacement origin,
    WorkspaceGridDirection direction) const
{
    origin = normalisePlacement(entry, origin);
    struct Candidate
    {
        WorkspaceGridPlacement placement;
        int directionalPenalty = 0;
        int orthogonalDistance = 0;
        int distance = 0;
    };

    std::vector<Candidate> candidates;
    const auto maximumRow = requiredGridRows() + static_cast<int>(entries.size()) * 4 + 8;
    for (int row = 0; row <= maximumRow; ++row)
    {
        for (int column = 0; column + origin.span.columns <= gridColumns; ++column)
        {
            WorkspaceGridPlacement placement { column, row, origin.span };
            if (placement == origin)
                continue;

            int penalty = 0;
            switch (direction)
            {
                case WorkspaceGridDirection::left: penalty = column > origin.column ? 1 : 0; break;
                case WorkspaceGridDirection::right: penalty = column < origin.column ? 1 : 0; break;
                case WorkspaceGridDirection::up: penalty = row > origin.row ? 1 : 0; break;
                case WorkspaceGridDirection::down: penalty = row < origin.row ? 1 : 0; break;
                case WorkspaceGridDirection::none: break;
            }
            const auto orthogonalDistance = direction == WorkspaceGridDirection::up
                                         || direction == WorkspaceGridDirection::down
                ? std::abs(column - origin.column)
                : direction == WorkspaceGridDirection::left
                       || direction == WorkspaceGridDirection::right
                    ? std::abs(row - origin.row)
                    : 0;
            candidates.push_back({ placement,
                                   penalty,
                                   orthogonalDistance,
                                   std::abs(row - origin.row) + std::abs(column - origin.column) });
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [] (const Candidate& left, const Candidate& right)
    {
        return std::tie(left.directionalPenalty,
                        left.orthogonalDistance,
                        left.distance,
                        left.placement.row,
                        left.placement.column)
             < std::tie(right.directionalPenalty,
                        right.orthogonalDistance,
                        right.distance,
                        right.placement.row,
                        right.placement.column);
    });
    for (const auto& candidate : candidates)
        if (placementIsFree(entry.descriptor.id, candidate.placement))
            return candidate.placement;
    return std::nullopt;
}

bool WorkspaceModuleLayout::entriesShareTile(const Entry& left, const Entry& right) const
{
    return !left.tabStackId.empty() && left.tabStackId == right.tabStackId;
}

void WorkspaceModuleLayout::setTilePlacement(WorkspaceModuleId id,
                                             WorkspaceGridPlacement placement)
{
    auto* entry = findEntry(id);
    if (entry == nullptr)
        return;
    if (entry->tabStackId.empty())
    {
        entry->placement = placement;
        return;
    }
    if (auto* stack = findTabStack(entry->tabStackId))
    {
        stack->placement = placement;
        for (const auto& tab : stack->tabInstanceIds)
        {
            if (auto* member = findEntryByInstanceId(tab))
                member->placement = placement;
            else if (auto opaque = std::find_if(inert.begin(), inert.end(), [&tab] (const InertModule& module)
                     {
                         return module.instanceId == tab;
                     }); opaque != inert.end())
                opaque->placement = placement;
        }
    }
}

void WorkspaceModuleLayout::setTileCustomized(WorkspaceModuleId id)
{
    auto* entry = findEntry(id);
    if (entry == nullptr)
        return;
    if (entry->tabStackId.empty())
    {
        entry->gridCustomized = true;
        return;
    }
    if (const auto* stack = findTabStack(entry->tabStackId))
        for (const auto& tab : stack->tabInstanceIds)
            if (auto* member = findEntryByInstanceId(tab))
                member->gridCustomized = true;
}

WorkspaceModuleLayout::TabStack* WorkspaceModuleLayout::findTabStack(const std::string& id)
{
    const auto found = std::find_if(stacks.begin(), stacks.end(), [&id] (const TabStack& stack)
    {
        return stack.id == id;
    });
    return found != stacks.end() ? &*found : nullptr;
}

const WorkspaceModuleLayout::TabStack* WorkspaceModuleLayout::findTabStack(const std::string& id) const
{
    const auto found = std::find_if(stacks.begin(), stacks.end(), [&id] (const TabStack& stack)
    {
        return stack.id == id;
    });
    return found != stacks.end() ? &*found : nullptr;
}

std::string WorkspaceModuleLayout::tileIdForModule(WorkspaceModuleId id) const
{
    if (id == WorkspaceModuleId::coreWaveform)
        return moduleKey(id);
    if (const auto* entry = findEntry(id))
        return entry->tabStackId.empty() ? entry->descriptor.instanceId : entry->tabStackId;
    return {};
}

WorkspaceModuleLayout::FloatingTile* WorkspaceModuleLayout::findFloatingTileForModule(
    WorkspaceModuleId id)
{
    const auto tileId = tileIdForModule(id);
    const auto found = std::find_if(floating.begin(), floating.end(), [&tileId] (const auto& tile)
    {
        return tile.id == tileId;
    });
    return found != floating.end() ? &*found : nullptr;
}

const WorkspaceModuleLayout::FloatingTile* WorkspaceModuleLayout::findFloatingTileForModule(
    WorkspaceModuleId id) const
{
    const auto tileId = tileIdForModule(id);
    const auto found = std::find_if(floating.begin(), floating.end(), [&tileId] (const auto& tile)
    {
        return tile.id == tileId;
    });
    return found != floating.end() ? &*found : nullptr;
}

bool WorkspaceModuleLayout::entryIsFloating(const Entry& entry) const
{
    const auto id = entry.tabStackId.empty() ? entry.descriptor.instanceId : entry.tabStackId;
    return std::any_of(floating.begin(), floating.end(), [&id] (const auto& tile)
    {
        return tile.id == id;
    });
}

void WorkspaceModuleLayout::removeFloatingTileForModule(WorkspaceModuleId id)
{
    const auto tileId = tileIdForModule(id);
    floating.erase(std::remove_if(floating.begin(), floating.end(), [&tileId] (const auto& tile)
    {
        return tile.id == tileId;
    }), floating.end());
}

void WorkspaceModuleLayout::replaceInactiveTabIfNeeded(const Entry& entry)
{
    if (entry.tabStackId.empty())
        return;
    auto* stack = findTabStack(entry.tabStackId);
    if (stack == nullptr || stack->activeInstanceId != entry.descriptor.instanceId)
        return;

    const auto replacement = std::find_if(stack->tabInstanceIds.begin(),
                                           stack->tabInstanceIds.end(),
                                           [this, &entry] (const std::string& instanceId)
    {
        if (instanceId == entry.descriptor.instanceId)
            return false;
        if (const auto* candidate = findEntryByInstanceId(instanceId))
            return candidate->available && candidate->enabled && candidate->placed;
        const auto opaque = std::find_if(inert.begin(), inert.end(), [&instanceId] (const InertModule& module)
        {
            return module.instanceId == instanceId;
        });
        return opaque != inert.end() && opaque->enabled && opaque->placed;
    });
    if (replacement != stack->tabInstanceIds.end())
        stack->activeInstanceId = *replacement;
}

void WorkspaceModuleLayout::removeModuleFromTabStack(WorkspaceModuleId id)
{
    auto* entry = findEntry(id);
    if (entry == nullptr || entry->tabStackId.empty())
        return;

    const auto stackId = entry->tabStackId;
    auto found = std::find_if(stacks.begin(), stacks.end(), [&stackId] (const TabStack& stack)
    {
        return stack.id == stackId;
    });
    if (found == stacks.end())
    {
        entry->tabStackId.clear();
        return;
    }

    const auto instanceId = entry->descriptor.instanceId;
    found->tabInstanceIds.erase(std::remove(found->tabInstanceIds.begin(),
                                             found->tabInstanceIds.end(),
                                             instanceId),
                                found->tabInstanceIds.end());
    entry->tabStackId.clear();
    if (found->tabInstanceIds.empty())
    {
        stacks.erase(found);
        return;
    }
    if (found->activeInstanceId == instanceId)
        found->activeInstanceId = found->tabInstanceIds.front();
    if (found->tabInstanceIds.size() == 1)
    {
        if (auto* remaining = findEntryByInstanceId(found->tabInstanceIds.front()))
        {
            remaining->tabStackId.clear();
            remaining->placement = found->placement;
        }
        else if (auto opaque = std::find_if(inert.begin(), inert.end(), [&found] (const InertModule& module)
                 {
                     return module.instanceId == found->tabInstanceIds.front();
                 }); opaque != inert.end())
        {
            opaque->tabStackId.clear();
            opaque->placement = found->placement;
        }
        stacks.erase(found);
    }
}

WorkspaceGridMutationResult WorkspaceModuleLayout::createOrMergeTabStack(WorkspaceModuleId id,
                                                                          WorkspaceModuleId target)
{
    auto* moving = findEntry(id);
    auto* targetEntry = findEntry(target);
    if (moving == nullptr || targetEntry == nullptr)
        return WorkspaceGridMutationResult::unknownModule;
    if (!moving->available || !moving->enabled || !moving->placed
        || !targetEntry->available || !targetEntry->enabled || !targetEntry->placed)
    {
        return WorkspaceGridMutationResult::moduleUnavailable;
    }
    if (!moving->tabStackId.empty() && moving->tabStackId == targetEntry->tabStackId)
        return WorkspaceGridMutationResult::noChange;

    const auto before = captureUserState();
    const auto trackSnapshot = captureTrackWeightSnapshot();
    const auto sourcePlacement = moving->placement;
    const auto targetPlacement = targetEntry->placement;
    const auto sourceStackId = moving->tabStackId;
    const auto movingInstanceId = moving->descriptor.instanceId;
    if (!sourceStackId.empty())
        removeModuleFromTabStack(id);

    moving = findEntry(id);
    targetEntry = findEntry(target);
    if (moving == nullptr || targetEntry == nullptr)
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::unknownModule;
    }

    auto targetStackId = targetEntry->tabStackId;
    if (targetStackId.empty())
    {
        targetStackId = "stack-" + std::to_string(nextStackSequence++);
        stacks.push_back({ targetStackId,
                           targetPlacement,
                           { targetEntry->descriptor.instanceId },
                           targetEntry->descriptor.instanceId });
        targetEntry->tabStackId = targetStackId;
    }

    auto* targetStack = findTabStack(targetStackId);
    if (targetStack == nullptr)
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::invalidPlacement;
    }

    const std::array<std::string, 1> movingTabs { movingInstanceId };
    for (const auto& tab : movingTabs)
    {
        if (std::find(targetStack->tabInstanceIds.begin(), targetStack->tabInstanceIds.end(), tab)
            == targetStack->tabInstanceIds.end())
        {
            targetStack->tabInstanceIds.push_back(tab);
        }
        if (auto* member = findEntryByInstanceId(tab))
        {
            member->tabStackId = targetStackId;
            member->placement = targetPlacement;
            member->gridCustomized = true;
        }
        else if (auto opaque = std::find_if(inert.begin(), inert.end(), [&tab] (const InertModule& module)
                 {
                     return module.instanceId == tab;
                 }); opaque != inert.end())
        {
            opaque->tabStackId = targetStackId;
            opaque->placement = targetPlacement;
        }
    }
    targetStack->activeInstanceId = moving->descriptor.instanceId;
    targetStack->placement = targetPlacement;
    targetEntry = findEntry(target);
    if (targetEntry != nullptr)
        targetEntry->gridCustomized = true;

    if (!resolveCollisions(id, WorkspaceGridDirection::none))
    {
        restoreUserState(before);
        return WorkspaceGridMutationResult::noSpace;
    }
    if (!(sourcePlacement == targetPlacement))
    {
        fillVacatedTile(sourcePlacement, id);
        compactGridTracks(id);
    }
    fillAllGridHoles();
    preserveUnchangedTrackWeights(trackSnapshot);
    return commitMutation(before);
}

bool WorkspaceModuleLayout::resolveCollisions(WorkspaceModuleId target,
                                              WorkspaceGridDirection direction)
{
    constexpr int maximumPasses = 128;
    const auto* targetEntry = findEntry(target);
    for (int pass = 0; pass < maximumPasses; ++pass)
    {
        Entry* collider = nullptr;
        for (auto& entry : entries)
        {
            if (!entry.available || !entry.enabled || !entry.placed || entryIsFloating(entry))
                continue;
            if (entry.placement.overlaps(corePlacement))
            {
                if (entry.descriptor.id == target
                    || (targetEntry != nullptr && entriesShareTile(*targetEntry, entry)))
                    return false;
                collider = &entry;
                break;
            }
        }

        if (collider == nullptr)
        {
            for (auto& entry : entries)
            {
                if (!entry.available || !entry.enabled || !entry.placed || entryIsFloating(entry))
                    continue;
                const auto overlapsInert = std::any_of(inert.begin(), inert.end(), [&entry] (const InertModule& module)
                {
                    return module.enabled && module.placed && entry.placement.overlaps(module.placement);
                });
                if (!overlapsInert)
                    continue;
                if (entry.descriptor.id == target
                    || (targetEntry != nullptr && entriesShareTile(*targetEntry, entry)))
                {
                    return false;
                }
                collider = &entry;
                break;
            }
        }

        if (collider == nullptr)
        {
            for (size_t left = 0; left < entries.size() && collider == nullptr; ++left)
            {
                if (!entries[left].available || !entries[left].enabled || !entries[left].placed
                    || entryIsFloating(entries[left]))
                    continue;
                for (size_t right = left + 1; right < entries.size(); ++right)
                {
                    if (!entries[right].available || !entries[right].enabled || !entries[right].placed
                        || entryIsFloating(entries[right])
                        || entriesShareTile(entries[left], entries[right])
                        || !entries[left].placement.overlaps(entries[right].placement))
                    {
                        continue;
                    }
                    collider = entries[left].descriptor.id == target ? &entries[right]
                             : entries[right].descriptor.id == target ? &entries[left]
                             : &entries[right];
                    break;
                }
            }
        }

        if (collider == nullptr)
            return true;
        const auto next = nearestFreePlacement(*collider, collider->placement, direction);
        if (!next.has_value())
            return false;
        setTilePlacement(collider->descriptor.id, *next);
    }
    return false;
}

void WorkspaceModuleLayout::ensureActivePlacements()
{
    std::unordered_set<std::string> visitedStacks;
    for (auto* entry : entriesInOrder())
    {
        if (!entry->available || !entry->enabled || !entry->placed || entryIsFloating(*entry))
            continue;
        if (!entry->tabStackId.empty() && !visitedStacks.insert(entry->tabStackId).second)
            continue;
        entry->placement = normalisePlacement(*entry, entry->placement);
        setTilePlacement(entry->descriptor.id, entry->placement);
        if (placementIsFree(entry->descriptor.id, entry->placement))
            continue;
        if (const auto free = nearestFreePlacement(*entry,
                                                   entry->placement,
                                                   WorkspaceGridDirection::down))
        {
            setTilePlacement(entry->descriptor.id, *free);
        }
    }
}

void WorkspaceModuleLayout::reflowDefaultPlacements()
{
    if (!(corePlacement == WorkspaceGridPlacement { 0, 0, { 3, 2 } }))
        return;
    if (!stacks.empty() || !inert.empty())
        return;

    std::vector<Entry*> active;
    for (auto* entry : entriesInOrder())
    {
        if (!entry->available || !entry->enabled || !entry->placed || entryIsFloating(*entry))
            continue;
        if (entry->gridCustomized)
            return;
        // This fast path only packs the one-column rail next to the default
        // core. Wider tools already have collision-safe placements from
        // ensureActivePlacements(); do not overwrite them across the core.
        if (entry->descriptor.preferredSpan.columns > gridColumns - corePlacement.right())
            return;
        active.push_back(entry);
    }
    if (active.empty())
        return;

    auto row = 0;
    for (auto* entry : active)
    {
        auto span = entry->descriptor.preferredSpan;
        if (active.size() == 1)
            span.rows = std::clamp(2,
                                   entry->descriptor.minimumSpan.rows,
                                   entry->descriptor.maximumSpan.rows);
        entry->placement = { gridColumns - span.columns, row, span };
        row += span.rows;
    }
}

void WorkspaceModuleLayout::compactGridTracks(std::optional<WorkspaceModuleId> excludedModule)
{
    const auto trackSnapshot = captureTrackWeightSnapshot();
    std::array<bool, gridColumns> usedColumns {};
    auto maximumRow = 0;
    const auto items = gridItems();
    const auto isExcludedTile = [excludedModule] (const WorkspaceGridItem& item)
    {
        return excludedModule.has_value() && item.id == *excludedModule
            && item.tabMembers.size() < 2;
    };
    for (const auto& item : items)
    {
        if (isExcludedTile(item))
            continue;
        for (auto column = item.placement.column;
             column < std::min(gridColumns, item.placement.right()); ++column)
            usedColumns[static_cast<std::size_t>(column)] = true;
        maximumRow = std::max(maximumRow, item.placement.bottom());
    }
    for (const auto& module : inert)
    {
        if (!module.enabled || !module.placed)
            continue;
        for (auto column = module.placement.column;
             column < std::min(gridColumns, module.placement.right()); ++column)
            usedColumns[static_cast<std::size_t>(column)] = true;
        maximumRow = std::max(maximumRow, module.placement.bottom());
    }

    std::vector<bool> usedRows(static_cast<std::size_t>(std::max(1, maximumRow)), false);
    const auto markRows = [&usedRows] (const WorkspaceGridPlacement& placement)
    {
        for (auto row = placement.row; row < placement.bottom(); ++row)
            if (juce::isPositiveAndBelow(row, static_cast<int>(usedRows.size())))
                usedRows[static_cast<std::size_t>(row)] = true;
    };
    for (const auto& item : items)
        if (!isExcludedTile(item))
            markRows(item.placement);
    for (const auto& module : inert)
        if (module.enabled && module.placed)
            markRows(module.placement);

    const auto compact = [&usedColumns, &usedRows] (WorkspaceGridPlacement placement)
    {
        const auto oldRight = placement.right();
        const auto oldBottom = placement.bottom();
        auto column = 0;
        auto columns = 0;
        for (auto candidate = 0; candidate < oldRight; ++candidate)
        {
            if (!usedColumns[static_cast<std::size_t>(candidate)])
                continue;
            if (candidate < placement.column)
                ++column;
            else
                ++columns;
        }
        auto row = 0;
        auto rows = 0;
        for (auto candidate = 0; candidate < oldBottom; ++candidate)
        {
            if (!usedRows[static_cast<std::size_t>(candidate)])
                continue;
            if (candidate < placement.row)
                ++row;
            else
                ++rows;
        }
        return WorkspaceGridPlacement { column, row,
                                        { std::max(1, columns), std::max(1, rows) } };
    };

    for (const auto& item : items)
    {
        if (isExcludedTile(item))
            continue;
        const auto placement = compact(item.placement);
        if (item.id == WorkspaceModuleId::coreWaveform)
            corePlacement = placement;
        else
            setTilePlacement(item.id, placement);
    }
    for (auto& module : inert)
        if (module.enabled && module.placed)
            module.placement = compact(module.placement);
    preserveUnchangedTrackWeights(trackSnapshot);
}

void WorkspaceModuleLayout::fillVacatedTile(WorkspaceGridPlacement hole,
                                             WorkspaceModuleId excludedModule)
{
    const auto items = gridItems();
    const auto isExcludedTile = [excludedModule] (const WorkspaceGridItem& item)
    {
        return item.id == excludedModule && item.tabMembers.size() < 2;
    };
    if (std::any_of(items.begin(), items.end(), [&] (const auto& item)
        {
            return !isExcludedTile(item) && item.placement == hole;
        }))
        return;

    const WorkspaceGridItem* best = nullptr;
    WorkspaceGridPlacement replacement;
    auto bestScore = std::numeric_limits<int>::max();
    for (const auto& item : items)
    {
        if (isExcludedTile(item))
            continue;
        auto candidate = item.placement;
        auto score = std::numeric_limits<int>::max();
        if (candidate.column == hole.column
            && candidate.span.columns == hole.span.columns
            && candidate.bottom() == hole.row)
        {
            candidate.span.rows += hole.span.rows;
            score = 0;
        }
        else if (candidate.column == hole.column
                 && candidate.span.columns == hole.span.columns
                 && candidate.row == hole.bottom())
        {
            candidate.row = hole.row;
            candidate.span.rows += hole.span.rows;
            score = 1;
        }
        else if (candidate.row == hole.row
                 && candidate.span.rows == hole.span.rows
                 && candidate.right() == hole.column)
        {
            candidate.span.columns += hole.span.columns;
            score = 2;
        }
        else if (candidate.row == hole.row
                 && candidate.span.rows == hole.span.rows
                 && candidate.column == hole.right())
        {
            candidate.column = hole.column;
            candidate.span.columns += hole.span.columns;
            score = 3;
        }
        if (score == std::numeric_limits<int>::max())
            continue;
        if (item.id == WorkspaceModuleId::coreWaveform)
            score += 10;
        if (score < bestScore)
        {
            best = &item;
            replacement = candidate;
            bestScore = score;
        }
    }

    if (best == nullptr)
        return;
    if (best->id == WorkspaceModuleId::coreWaveform)
        corePlacement = replacement;
    else
        setTilePlacement(best->id, replacement);
}

void WorkspaceModuleLayout::fillAllGridHoles()
{
    compactGridTracks();
    constexpr auto maximumPasses = 512;
    for (auto pass = 0; pass < maximumPasses; ++pass)
    {
        const auto items = gridItems();
        auto columns = 0;
        auto rows = 0;
        for (const auto& item : items)
        {
            columns = std::max(columns, item.placement.right());
            rows = std::max(rows, item.placement.bottom());
        }
        if (columns <= 0 || rows <= 0)
            return;

        std::vector<int> occupancy(static_cast<std::size_t>(columns * rows), -1);
        for (size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
            for (auto row = items[itemIndex].placement.row;
                 row < items[itemIndex].placement.bottom(); ++row)
                for (auto column = items[itemIndex].placement.column;
                     column < items[itemIndex].placement.right(); ++column)
                    if (column >= 0 && column < columns && row >= 0 && row < rows)
                        occupancy[static_cast<std::size_t>(row * columns + column)]
                            = static_cast<int>(itemIndex);

        const auto stripIsEmpty = [&] (const WorkspaceGridPlacement& placement,
                                       WorkspaceGridDirection direction)
        {
            if (direction == WorkspaceGridDirection::down
                || direction == WorkspaceGridDirection::up)
            {
                const auto row = direction == WorkspaceGridDirection::down
                    ? placement.bottom() : placement.row - 1;
                if (row < 0 || row >= rows)
                    return false;
                for (auto column = placement.column; column < placement.right(); ++column)
                    if (occupancy[static_cast<std::size_t>(row * columns + column)] >= 0)
                        return false;
                return true;
            }
            const auto column = direction == WorkspaceGridDirection::right
                ? placement.right() : placement.column - 1;
            if (column < 0 || column >= columns)
                return false;
            for (auto row = placement.row; row < placement.bottom(); ++row)
                if (occupancy[static_cast<std::size_t>(row * columns + column)] >= 0)
                    return false;
            return true;
        };

        // A top-left hole is not necessarily adjacent to a tile that can grow.
        // For example, a two-column EQ inserted below the waveform may leave a
        // two-cell gap to its left.  Grow into the first *fillable* hole, then
        // restart; the EQ can consume the nearer cell first and the farther
        // cell on the next pass.  Stopping at the first unfillable cell leaves
        // a visible blank even though a deterministic rectangular fill exists.
        const WorkspaceGridItem* best = nullptr;
        WorkspaceGridPlacement replacement;
        auto foundAnyHole = false;
        for (auto holeRow = 0; holeRow < rows && best == nullptr; ++holeRow)
        {
            for (auto holeColumn = 0; holeColumn < columns && best == nullptr; ++holeColumn)
            {
                if (occupancy[static_cast<std::size_t>(holeRow * columns + holeColumn)] >= 0)
                    continue;
                foundAnyHole = true;

                auto bestScore = std::numeric_limits<int>::max();
                for (const auto& item : items)
                {
                    auto candidate = item.placement;
                    auto score = std::numeric_limits<int>::max();
                    if (candidate.bottom() == holeRow
                        && holeColumn >= candidate.column && holeColumn < candidate.right()
                        && stripIsEmpty(candidate, WorkspaceGridDirection::down))
                    {
                        ++candidate.span.rows;
                        score = 0;
                    }
                    else if (candidate.row == holeRow + 1
                             && holeColumn >= candidate.column && holeColumn < candidate.right()
                             && stripIsEmpty(candidate, WorkspaceGridDirection::up))
                    {
                        --candidate.row;
                        ++candidate.span.rows;
                        score = 1;
                    }
                    else if (candidate.right() == holeColumn
                             && holeRow >= candidate.row && holeRow < candidate.bottom()
                             && stripIsEmpty(candidate, WorkspaceGridDirection::right))
                    {
                        ++candidate.span.columns;
                        score = 2;
                    }
                    else if (candidate.column == holeColumn + 1
                             && holeRow >= candidate.row && holeRow < candidate.bottom()
                             && stripIsEmpty(candidate, WorkspaceGridDirection::left))
                    {
                        --candidate.column;
                        ++candidate.span.columns;
                        score = 3;
                    }
                    if (score == std::numeric_limits<int>::max())
                        continue;
                    if (item.id == WorkspaceModuleId::coreWaveform)
                        score += 5;
                    if (score < bestScore)
                    {
                        best = &item;
                        replacement = candidate;
                        bestScore = score;
                    }
                }
            }
        }
        if (!foundAnyHole)
            return;
        if (best == nullptr)
            return;
        if (best->id == WorkspaceModuleId::coreWaveform)
            corePlacement = replacement;
        else
            setTilePlacement(best->id, replacement);
    }
}

void WorkspaceModuleLayout::fillLocalHole(WorkspaceGridPlacement hole,
                                          WorkspaceModuleId protectedTarget)
{
    std::unordered_set<std::string> movedTiles;
    for (size_t pass = 0; pass < entries.size(); ++pass)
    {
        Entry* best = nullptr;
        int bestDistance = std::numeric_limits<int>::max();
        for (auto& entry : entries)
        {
            const auto tileKey = entry.tabStackId.empty()
                ? entry.descriptor.instanceId
                : entry.tabStackId;
            if (!entry.available || !entry.enabled || !entry.placed || entryIsFloating(entry)
                || entry.descriptor.id == protectedTarget
                || movedTiles.find(tileKey) != movedTiles.end()
                || (!entry.tabStackId.empty()
                    && findEntry(protectedTarget) != nullptr
                    && entry.tabStackId == findEntry(protectedTarget)->tabStackId)
                || entry.placement.span.columns > hole.span.columns
                || entry.placement.span.rows > hole.span.rows)
            {
                continue;
            }
            const auto distance = std::abs(entry.placement.row - hole.row)
                                + std::abs(entry.placement.column - hole.column);
            if (distance < bestDistance
                || (distance == bestDistance && best != nullptr
                    && std::tie(entry.placement.row, entry.placement.column, entry.order)
                     < std::tie(best->placement.row, best->placement.column, best->order)))
            {
                WorkspaceGridPlacement candidate { hole.column, hole.row, entry.placement.span };
                if (placementIsFree(entry.descriptor.id, candidate))
                {
                    best = &entry;
                    bestDistance = distance;
                }
            }
        }
        if (best == nullptr)
            return;
        const auto nextHole = best->placement;
        setTilePlacement(best->descriptor.id, { hole.column, hole.row, best->placement.span });
        movedTiles.insert(best->tabStackId.empty() ? best->descriptor.instanceId : best->tabStackId);
        hole = nextHole;
    }
}

WorkspaceGridMutationResult WorkspaceModuleLayout::commitMutation(const UserState& before)
{
    const auto after = captureUserState();
    if (userStateEquals(before, after))
        return WorkspaceGridMutationResult::noChange;
    undoHistory.push_back(before);
    redoHistory.clear();
    return WorkspaceGridMutationResult::committed;
}

WorkspaceModuleLayout::Entry* WorkspaceModuleLayout::findEntry(WorkspaceModuleId id)
{
    const auto found = std::find_if(entries.begin(), entries.end(), [id] (const Entry& entry)
    {
        return entry.descriptor.id == id;
    });
    return found != entries.end() ? &*found : nullptr;
}

const WorkspaceModuleLayout::Entry* WorkspaceModuleLayout::findEntry(WorkspaceModuleId id) const
{
    const auto found = std::find_if(entries.begin(), entries.end(), [id] (const Entry& entry)
    {
        return entry.descriptor.id == id;
    });
    return found != entries.end() ? &*found : nullptr;
}

WorkspaceModuleLayout::Entry* WorkspaceModuleLayout::findEntryByInstanceId(const std::string& id)
{
    const auto found = std::find_if(entries.begin(), entries.end(), [&id] (const Entry& entry)
    {
        return entry.descriptor.instanceId == id;
    });
    return found != entries.end() ? &*found : nullptr;
}

const WorkspaceModuleLayout::Entry* WorkspaceModuleLayout::findEntryByInstanceId(const std::string& id) const
{
    const auto found = std::find_if(entries.begin(), entries.end(), [&id] (const Entry& entry)
    {
        return entry.descriptor.instanceId == id;
    });
    return found != entries.end() ? &*found : nullptr;
}

std::vector<WorkspaceModuleLayout::Entry*> WorkspaceModuleLayout::entriesInOrder()
{
    std::vector<Entry*> ordered;
    ordered.reserve(entries.size());
    for (auto& entry : entries)
        ordered.push_back(&entry);
    std::stable_sort(ordered.begin(), ordered.end(), [] (const Entry* left, const Entry* right)
    {
        return left->order < right->order;
    });
    return ordered;
}

std::vector<const WorkspaceModuleLayout::Entry*> WorkspaceModuleLayout::entriesInOrder() const
{
    std::vector<const Entry*> ordered;
    ordered.reserve(entries.size());
    for (const auto& entry : entries)
        ordered.push_back(&entry);
    std::stable_sort(ordered.begin(), ordered.end(), [] (const Entry* left, const Entry* right)
    {
        return left->order < right->order;
    });
    return ordered;
}
