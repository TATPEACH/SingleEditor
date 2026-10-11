#pragma once

#include "Audio/ChannelLayout.h"
#include "Audio/DeviceOutput.h"
#include <cstdint>
#include <memory>

namespace singleeditor
{
// D105: a unity/binary connection plan after the canonical speaker bus. It
// neither decodes/downmixes nor owns device rate, gain, EQ or document state.
class DeviceRoutePlan final
{
public:
    static constexpr int maxBusChannels = 32;
    static constexpr int maxDeviceChannels = 512;
    struct Speaker
    {
        std::string stableId;
        SpeakerRole role = SpeakerRole::unknown;
        bool required = true;
        bool operator==(const Speaker&) const = default;
    };
    struct Bus
    {
        std::string profileId;
        std::int64_t revision = 0;
        std::vector<Speaker> speakers; // exact canonical render order
        bool operator==(const Bus&) const = default;
    };
    struct Draft
    {
        std::string backend, stableId;
        std::vector<std::string> outputNames; // full observed capability order
        Bus bus;
        std::vector<std::vector<int>> outputsBySpeaker; // absolute physical indices
    };
    enum class IssueCode
    {
        invalidDevice, staleDevice, invalidBus, staleGeometry, invalidMatrixShape,
        inactiveOutput, duplicateConnection, requiredUnmapped, convergentOutput,
        possibleClipping, crossedRole
    };
    struct Issue
    {
        IssueCode code;
        bool fatal = false;
        int speakerRow = -1, physicalOutput = -1;
    };
    class Review final
    {
    public:
        const std::vector<Issue>& issues() const noexcept { return found; }
        bool isValid() const noexcept { return candidate != nullptr; }
        bool needsConfirmation() const noexcept;
        // Confirmation is scoped to this immutable review, never a mutable draft.
        std::shared_ptr<const DeviceRoutePlan> approved(bool explicitlyConfirmWarnings = false) const noexcept;
    private:
        friend class DeviceRoutePlan;
        std::vector<Issue> found;
        std::shared_ptr<const DeviceRoutePlan> candidate;
    };
    // Supply roles only from authoritative backend layout metadata. Names,
    // counts and saved indices are not trustworthy role semantics.
    using TrustedRoles = std::optional<std::vector<SpeakerRole>>;
    static Draft propose(const Bus&, const DeviceOutput::Snapshot&, const TrustedRoles& = {});
    static Review validate(const Draft&, const DeviceOutput::Snapshot&, const Bus& currentBus, const TrustedRoles& = {});
    bool matches(const DeviceOutput::Snapshot&, const Bus& currentBus, const TrustedRoles& = {}) const noexcept;
    // Keep the exact approved connections and warning consent. Only observed
    // stream format/latency may change; identity, geometry and ports must match.
    std::shared_ptr<const DeviceRoutePlan> withActualFormat(const DeviceOutput::Snapshot& actual,
        const Bus& currentBus, const TrustedRoles& roles = {}) const
    {
        if (!matches(actual, currentBus, roles)) return {};
        auto updated = std::shared_ptr<DeviceRoutePlan>(new DeviceRoutePlan(*this));
        updated->target = actual;
        return updated;
    }
    const Bus& bus() const noexcept { return canonical; }
    // Reuse only the approved routing/consent for an explicit rate request.
    // This is not a statement about an opened device; D106 must still verify it.
    std::shared_ptr<const DeviceRoutePlan> withRequestedSampleRate(double rate, const Bus& currentBus) const
    {
        auto requested = target;
        requested.sampleRate = rate;
        return withActualFormat(requested, currentBus, semantics);
    }
    const ChannelLayoutState& canonicalLayout() const noexcept { return layout; }
    const DeviceOutput::Snapshot& device() const noexcept { return target; }
    const TrustedRoles& trustedOutputRoles() const noexcept { return semantics; }
    int outputSlotForPhysical(int physicalIndex) const noexcept;
    bool connected(int speakerRow, int outputSlot) const noexcept;
    // Preallocated, disjoint input/output buffers; no allocation, clipping,
    // resampling or normalization. Invalid dimensions produce silence.
    bool render(const float* const* canonicalInputs, int inputChannels,
                float* const* deviceOutputs, int outputChannels, int samples) const noexcept;
private:
    DeviceRoutePlan() = default;
    Bus canonical;
    ChannelLayoutState layout; // prepared on the control thread, never inferred from device width
    DeviceOutput::Snapshot target;
    TrustedRoles semantics;
    std::vector<std::uint8_t> matrix; // output-major, only 0 or 1
};
}
