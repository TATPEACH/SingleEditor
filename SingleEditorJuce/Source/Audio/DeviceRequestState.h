#pragma once
#include <juce_core/juce_core.h>
#include <mutex>
#include <optional>
#include <utility>

namespace singleeditor
{
enum class DeviceRequestKind { initialise, selectBackend, applySetup, enableOutputs,
    activateOutput, inspectOutput, inspectPanel, maintainOutput, showOutputPanel, suspendOutput };
enum class DeviceRequestOutcome { succeeded, rejected, noOpenDevice, stopped, pending, reviewed, cancelled, superseded };
enum class DeviceSwitchPhase { inactive, preparing, readyToSwitch, switching, active, failed, restoring, readyForReview };
enum class DeviceSwitchError { none, invalidSelection, engineBusy, launchFailed, cancelled, superseded,
    closeFailed, prepareFailed, staleCandidate, commitFailed, restoreFailed, driverUnresolved, rebindRequired, circuitOpen };

struct DeviceRequestRecord
{
    juce::Uuid operationId { nullptr };
    DeviceRequestKind kind = DeviceRequestKind::initialise, stage = DeviceRequestKind::initialise;
    DeviceRequestOutcome outcome = DeviceRequestOutcome::succeeded;
    juce::String requestedBackend;
    double requestedRate = 0.0; // zero delegates selection; an inspection requests no format
    int requestedBuffer = 0, requestedOutputChannels = -1, followupOutputChannels = -1;
    bool defaultOutputChannels = true;
    bool direct = false;
    DeviceSwitchPhase switchPhase = DeviceSwitchPhase::inactive, failurePhase = DeviceSwitchPhase::inactive;
    DeviceSwitchError switchError = DeviceSwitchError::none;
    bool restorationAttempted = false, restored = false, resourcesReleased = true, awaitingPanel = false;
    juce::String requestedOutputIdentity; // owner-side equality only; never a public field

    bool failed() const noexcept
    {
        return outcome == DeviceRequestOutcome::rejected || outcome == DeviceRequestOutcome::noOpenDevice
            || (direct && resourcesReleased && restorationAttempted && !restored);
    }
};

// Two value capsules owned by the loaded audio owner, created only by an actual
// device operation. Workers may publish without retaining AudioEngine, a driver,
// a coordinator, or an event history. Observation never creates this state.
class DeviceRequestState final
{
public:
    struct Snapshot { std::optional<DeviceRequestRecord> latest, failure; };
    void begin(DeviceRequestRecord request)
    {
        const std::lock_guard lock(mutex);
        values.latest = std::move(request);
        if (values.latest->failed()) values.failure = values.latest;
    }
    template<class ReadCurrent>
    void publish(const juce::Uuid& operationId, ReadCurrent&& readCurrent, bool newFailure = false)
    {
        const std::lock_guard lock(mutex);
        // Read the operation values while holding this publication lock so a
        // late poll cannot replace a newer terminal publication with old data.
        auto current = readCurrent();
        if (values.latest && values.latest->operationId == operationId) values.latest = current;
        // A genuine later failure of an older in-flight operation is still a
        // new failure. Mere cleanup of an old failure cannot overwrite a newer
        // capsule; current request and latest failure have independent IDs.
        if (current.failed() && (newFailure || (values.failure && values.failure->operationId == operationId)))
            values.failure = std::move(current);
    }
    std::optional<Snapshot> snapshot() const
    {
        const std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) return {};
        return values;
    }
private:
    mutable std::mutex mutex;
    Snapshot values;
};
}
