#pragma once
#include "Audio/AudioEngine.h"
#include "Audio/DeviceBackendCapabilities.h"
#include <functional>

namespace singleeditor
{
// Application-owned operation, created only by explicit device intent. The
// owner polls while an operation is pending; there is no permanent timer.
// Driver prepare/close/restore/disposal run on one worker, with no AudioEngine
// pointer on that worker. Owner-thread callback handoff uses the exact lease.
class DeviceSwitchCoordinator final
{
public:
    using Phase = DeviceSwitchPhase;
    enum class Mode { parallel, exclusive };
    using Error = DeviceSwitchError;
    struct Selection
    {
        std::uint64_t generation = 0;
        DeviceOutput::Snapshot expected;
        DeviceRoutePlan::Bus bus;
        DeviceRoutePlan::TrustedRoles roles;
        std::shared_ptr<const DeviceRoutePlan> plan;
    };
    struct Prepared
    {
        std::unique_ptr<DeviceOutput> output;
        Error error = Error::none;
    };
    using Prepare = std::function<Prepared(const Selection&, const DeviceSwitchAccess&, const std::atomic<bool>&)>;
    struct InspectionSelection
    {
        std::uint64_t generation = 0;
        std::string backend, stableId;
    };
    // These are detached backend values,
    // never a JUCE device/type or permission to activate a route.
    using InspectedCapabilities = PreparedDeviceCapabilities;
    struct Inspected
    {
        std::shared_ptr<const InspectedCapabilities> capabilities;
        Error error = Error::none;
    };
    using Inspect = std::function<Inspected(const InspectionSelection&, const DeviceSwitchAccess&, const std::atomic<bool>&)>;
    using PanelInspect = std::function<Inspected(const InspectionSelection&, const DeviceSwitchAccess&,
        const std::atomic<bool>&, std::atomic<bool>& awaitingPanel)>;
    struct Snapshot
    {
        Phase phase = Phase::inactive;
        Error error = Error::none;
        std::uint64_t generation = 0;
        std::string backend, stableId;
        bool outputSuspended = false, restorationAttempted = false, restored = false;
        bool driverUnresolved = false, resourcesReleased = false;
        bool awaitingPanel = false;
    };

    static std::shared_ptr<DeviceSwitchCoordinator> start(AudioEngine&, Selection, Mode, Prepare);
    static std::shared_ptr<DeviceSwitchCoordinator> startInspection(AudioEngine&, InspectionSelection, Inspect);
    static std::shared_ptr<DeviceSwitchCoordinator> startPanelInspection(AudioEngine&, InspectionSelection, PanelInspect);
    static std::shared_ptr<DeviceSwitchCoordinator> startMaintenance(AudioEngine&, Selection, bool showPanel, bool stopOnly = false, bool requireRebind = false);
    ~DeviceSwitchCoordinator();
    void poll(AudioEngine&, const Selection& currentSelection); // owner thread, current independent selection
    void pollInspection(AudioEngine&, const InspectionSelection& currentSelection);
    // Published only after the temporary driver and old-owner return have
    // drained. A stale selection, cancel or failed restoration publishes none.
    std::shared_ptr<const InspectedCapabilities> inspection(const InspectionSelection& currentSelection) const;
    void cancel() noexcept;
    void abandon() noexcept; // application/engine teardown only; a settings dialog must only cancel
    Snapshot snapshot() const;
private:
    struct State;
    explicit DeviceSwitchCoordinator(std::shared_ptr<State>);
    static void run(const std::shared_ptr<State>&, Prepare);
    static std::shared_ptr<DeviceSwitchCoordinator> startOperation(AudioEngine&, Selection, Mode, Prepare, Inspect,
        bool maintaining = false, bool showPanel = false, bool stopOnly = false, bool requireRebind = false,
        PanelInspect panelInspect = {});
    std::shared_ptr<State> state;
};
}
