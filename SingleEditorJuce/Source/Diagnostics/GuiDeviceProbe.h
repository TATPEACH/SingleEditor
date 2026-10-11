#pragma once
#include "Audio/AudioEngine.h"
#include "DeviceDiagnosticProbe.h"

namespace singleeditor::diagnostics
{
// Created only after explicit effect consent. Main owns restoration; a dialog
// may request cancellation but cannot destroy the engine's leased manager.
class GuiDeviceProbe final
{
public:
    enum class Phase { preparing, closing, probing, restoring, awaitingReturn, finished, abandoned };
    static std::shared_ptr<GuiDeviceProbe> start(AudioEngine&, const AudioEngine::DeviceProbeConfiguration&,
        const DeviceProbeRequest&, const juce::File& workerExecutable);
    ~GuiDeviceProbe();
    void cancel() noexcept;
    void abandon() noexcept; // owner thread teardown; no join and no engine on worker
    void pollReturn(AudioEngine&); // exact owner, message thread only
    Phase phase() const noexcept;
    bool cancellationRequested() const noexcept;
    bool driverUnresolved() const noexcept;
    bool resourcesReleased() const noexcept;
    const DeviceProbeResult* result() const noexcept; // only after finished, immutable
private:
    struct State;
    explicit GuiDeviceProbe(std::shared_ptr<State>);
    std::shared_ptr<State> state;
};
}
