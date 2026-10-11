#include "Audio/DeviceRoutePlan.h"
#include <algorithm>
#include <unordered_set>

namespace singleeditor
{
namespace
{
bool namedRole(SpeakerRole role) noexcept
{ return role > SpeakerRole::discrete && role < SpeakerRole::ambiACN0; }
bool speakerRole(SpeakerRole role) noexcept
{ return role >= SpeakerRole::unknown && role < SpeakerRole::ambiACN0; }
bool validDevice(const DeviceOutput::Snapshot& device)
{
    const auto& active = device.physicalOutputs;
    return !device.backend.empty() && !device.stableId.empty() && !device.outputNames.empty()
        && device.outputNames.size() <= DeviceRoutePlan::maxDeviceChannels
        && !active.empty() && active.size() <= DeviceRoutePlan::maxBusChannels
        && std::is_sorted(active.begin(), active.end()) && active.front() >= 0
        && active.back() < static_cast<int>(device.outputNames.size())
        && std::adjacent_find(active.begin(), active.end()) == active.end();
}
}

bool DeviceRoutePlan::Review::needsConfirmation() const noexcept
{ return std::any_of(found.begin(), found.end(), [](const auto& issue) { return !issue.fatal; }); }

std::shared_ptr<const DeviceRoutePlan> DeviceRoutePlan::Review::approved(bool confirmed) const noexcept
{ return candidate && (confirmed || !needsConfirmation()) ? candidate : nullptr; }

DeviceRoutePlan::Draft DeviceRoutePlan::propose(const Bus& bus, const DeviceOutput::Snapshot& device, const TrustedRoles& trusted)
{
    Draft draft {device.backend, device.stableId, device.outputNames, bus, {}};
    draft.outputsBySpeaker.resize(bus.speakers.size());
    if (!validDevice(device) || !trusted || trusted->size() != device.outputNames.size()) return draft;
    for (std::size_t row = 0; row < bus.speakers.size(); ++row)
    {
        const auto role = bus.speakers[row].role;
        if (!namedRole(role) || std::count_if(bus.speakers.begin(), bus.speakers.end(),
                [role](const auto& speaker) { return speaker.role == role; }) != 1) continue;
        int match = -1, count = 0;
        for (const auto physical : device.physicalOutputs)
            if ((*trusted)[static_cast<std::size_t>(physical)] == role) { match = physical; ++count; }
        if (count == 1) draft.outputsBySpeaker[row].push_back(match);
    }
    return draft;
}

DeviceRoutePlan::Review DeviceRoutePlan::validate(const Draft& draft, const DeviceOutput::Snapshot& device,
    const Bus& bus, const TrustedRoles& trusted)
{
    Review review;
    const auto issue = [&](IssueCode code, bool fatal, int row = -1, int physical = -1)
    { review.found.push_back({code, fatal, row, physical}); };
    if (!validDevice(device)) issue(IssueCode::invalidDevice, true);
    if (trusted && (trusted->size() != device.outputNames.size()
        || std::any_of(trusted->begin(), trusted->end(), [](auto role) { return !speakerRole(role); })))
        issue(IssueCode::invalidDevice, true);
    if (draft.backend != device.backend || draft.stableId != device.stableId || draft.outputNames != device.outputNames)
        issue(IssueCode::staleDevice, true);
    if (draft.bus != bus) issue(IssueCode::staleGeometry, true);
    std::unordered_set<std::string> ids;
    if (bus.profileId.empty() || bus.revision <= 0 || bus.speakers.empty() || bus.speakers.size() > maxBusChannels)
        issue(IssueCode::invalidBus, true);
    for (const auto& speaker : bus.speakers)
        if (speaker.stableId.empty() || !speakerRole(speaker.role) || !ids.insert(speaker.stableId).second) issue(IssueCode::invalidBus, true);
    if (draft.outputsBySpeaker.size() != bus.speakers.size()) issue(IssueCode::invalidMatrixShape, true);
    if (std::any_of(draft.outputsBySpeaker.begin(), draft.outputsBySpeaker.end(), [](const auto& outputs) { return outputs.size() > maxDeviceChannels; }))
        issue(IssueCode::invalidMatrixShape, true);
    if (std::any_of(review.found.begin(), review.found.end(), [](const auto& found) { return found.fatal; })) return review;

    auto plan = std::shared_ptr<DeviceRoutePlan>(new DeviceRoutePlan());
    plan->canonical = bus; plan->target = device; plan->semantics = trusted;
    plan->layout.sourceChannelCount = static_cast<int>(bus.speakers.size());
    plan->layout.selectedPresetId = bus.profileId;
    plan->layout.interpretationOrigin = ChannelInterpretationOrigin::explicitPreset;
    for (const auto& speaker : bus.speakers) plan->layout.selectedRoles.push_back(speaker.role);
    const auto rows = bus.speakers.size();
    plan->matrix.assign(rows * device.physicalOutputs.size(), 0);
    for (std::size_t row = 0; row < rows; ++row)
    {
        if (draft.outputsBySpeaker[row].empty() && bus.speakers[row].required)
            issue(IssueCode::requiredUnmapped, false, static_cast<int>(row));
        for (const auto physical : draft.outputsBySpeaker[row])
        {
            const auto slot = plan->outputSlotForPhysical(physical);
            if (slot < 0) { issue(IssueCode::inactiveOutput, true, static_cast<int>(row), physical); continue; }
            auto& cell = plan->matrix[static_cast<std::size_t>(slot) * rows + row];
            if (cell != 0) { issue(IssueCode::duplicateConnection, false, static_cast<int>(row), physical); continue; }
            cell = 1;
            if (trusted && trusted->size() == device.outputNames.size()
                && namedRole(bus.speakers[row].role) && namedRole((*trusted)[static_cast<std::size_t>(physical)])
                && bus.speakers[row].role != (*trusted)[static_cast<std::size_t>(physical)])
                issue(IssueCode::crossedRole, false, static_cast<int>(row), physical);
        }
    }
    for (std::size_t slot = 0; slot < device.physicalOutputs.size(); ++slot)
    {
        int contributors = 0;
        for (std::size_t row = 0; row < rows; ++row) contributors += plan->matrix[slot * rows + row];
        if (contributors > 1)
        {
            issue(IssueCode::convergentOutput, false, -1, device.physicalOutputs[slot]);
            issue(IssueCode::possibleClipping, false, -1, device.physicalOutputs[slot]);
        }
    }
    if (std::none_of(review.found.begin(), review.found.end(), [](const auto& found) { return found.fatal; }))
        review.candidate = std::move(plan);
    return review;
}

bool DeviceRoutePlan::matches(const DeviceOutput::Snapshot& device, const Bus& bus, const TrustedRoles& trusted) const noexcept
{
    return canonical == bus && target.backend == device.backend && target.stableId == device.stableId
        && target.outputNames == device.outputNames && target.physicalOutputs == device.physicalOutputs && semantics == trusted;
}

int DeviceRoutePlan::outputSlotForPhysical(int physical) const noexcept
{
    const auto it = std::lower_bound(target.physicalOutputs.begin(), target.physicalOutputs.end(), physical);
    return it != target.physicalOutputs.end() && *it == physical ? static_cast<int>(it - target.physicalOutputs.begin()) : -1;
}

bool DeviceRoutePlan::connected(int row, int slot) const noexcept
{
    return row >= 0 && row < static_cast<int>(canonical.speakers.size())
        && slot >= 0 && slot < static_cast<int>(target.physicalOutputs.size())
        && matrix[static_cast<std::size_t>(slot) * canonical.speakers.size() + static_cast<std::size_t>(row)] != 0;
}

bool DeviceRoutePlan::render(const float* const* inputs, int inputChannels, float* const* outputs, int outputChannels, int samples) const noexcept
{
    if (!outputs || outputChannels < 0 || outputChannels > maxDeviceChannels || samples < 0) return false;
    for (int output = 0; output < outputChannels; ++output)
        if (outputs[output]) std::fill_n(outputs[output], samples, 0.0f);
    if (!inputs || inputChannels != static_cast<int>(canonical.speakers.size())
        || outputChannels != static_cast<int>(target.physicalOutputs.size())) return false;
    for (int input = 0; input < inputChannels; ++input) if (!inputs[input]) return false;
    for (int output = 0; output < outputChannels; ++output) if (!outputs[output]) return false;
    for (int output = 0; output < outputChannels; ++output)
        for (int input = 0; input < inputChannels; ++input)
            if (connected(input, output))
                for (int sample = 0; sample < samples; ++sample) outputs[output][sample] += inputs[input][sample];
    return true;
}
}
