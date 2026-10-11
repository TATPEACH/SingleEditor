#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "Audio/AmbisonicFieldAnalyzer.h"
#include "Audio/AudioDeviceWarmupTask.h"
#include "Audio/ChannelLayout.h"
#include "Audio/ChannelRouter.h"
#include "Audio/DeviceOutput.h"
#include "Audio/DeviceRequestState.h"
#include "Audio/DeviceRoutePlan.h"
#include "Audio/DeviceSwitchAccess.h"
#include "Audio/OutputMeterAnalyzer.h"
#include "Audio/PlaybackRateSource.h"
#include "Audio/SpatialEnergyService.h"
#include "Document/SingleTrackDocument.h"
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor
{
struct AudioSuitePreviewState;
struct AmbisonicDecodeMatrixArtifact;
class DeviceSwitchCoordinator;

class AudioEngine final : private SingleTrackDocument::Listener
{
    struct AmbisonicFieldLeaseControl;
    struct OutputMeterLeaseControl;
    using DeviceLeaseIdentity = DeviceSwitchAccess::State;

public:
    class OutputMeterAnalysisLease final
    {
    public:
        OutputMeterAnalysisLease() = default;
        OutputMeterAnalysisLease(OutputMeterAnalysisLease&&) noexcept;
        OutputMeterAnalysisLease& operator=(OutputMeterAnalysisLease&&) noexcept;
        ~OutputMeterAnalysisLease();
        OutputMeterAnalysisLease(const OutputMeterAnalysisLease&) = delete;
        OutputMeterAnalysisLease& operator=(const OutputMeterAnalysisLease&) = delete;
        explicit operator bool() const noexcept { return control != nullptr; }
    private:
        friend class AudioEngine;
        explicit OutputMeterAnalysisLease(std::shared_ptr<OutputMeterLeaseControl>) noexcept;
        void reset() noexcept;
        std::shared_ptr<OutputMeterLeaseControl> control;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        OptionalLifecycleToken optionalLifecycleToken;
#endif
    };

    class AmbisonicFieldAnalysisLease final
    {
    public:
        AmbisonicFieldAnalysisLease() = default;
        AmbisonicFieldAnalysisLease(AmbisonicFieldAnalysisLease&& other) noexcept;
        AmbisonicFieldAnalysisLease& operator=(AmbisonicFieldAnalysisLease&& other) noexcept;
        ~AmbisonicFieldAnalysisLease();

        AmbisonicFieldAnalysisLease(const AmbisonicFieldAnalysisLease&) = delete;
        AmbisonicFieldAnalysisLease& operator=(const AmbisonicFieldAnalysisLease&) = delete;

        explicit operator bool() const noexcept { return leaseId != 0; }

    private:
        friend class AudioEngine;
        AmbisonicFieldAnalysisLease(std::shared_ptr<AmbisonicFieldLeaseControl> controlToUse,
                                    std::uint64_t leaseIdToUse) noexcept;
        void reset() noexcept;

        std::shared_ptr<AmbisonicFieldLeaseControl> control;
        std::uint64_t leaseId = 0;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        OptionalLifecycleToken optionalLifecycleToken;
#endif
    };

    struct LoadedFileInfo
    {
        juce::File file;
        juce::String formatName;
        juce::StringPairArray metadata;
        double durationSeconds = 0.0;
        double sampleRate = 0.0;
        int numChannels = 0;
        int bitsPerSample = 0;
        int64_t audioRevision = 0;
        ChannelLayoutState channelLayout;

        bool isValid() const noexcept { return file.existsAsFile(); }
    };

    enum class PlaybackState
    {
        stopped,
        playing,
        paused
    };

    enum class MeterMode
    {
        truePeak = 1,
        rms,
        lufs
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void loadedFileChanged(const LoadedFileInfo&) {}
        virtual void playbackStateChanged(PlaybackState) {}
    };

    explicit AudioEngine(SingleTrackDocument& documentToUse);
    ~AudioEngine();

    bool loadFile(const juce::File& file);

    void play();
    void pause();
    void stop();
    void seekToSeconds(double seconds);
    void seekBySeconds(double deltaSeconds);
    void stepByFrames(int deltaFrames);

    void setLoopEnabled(bool shouldLoop) noexcept;
    bool isLoopEnabled() const noexcept;
    void setReturnToPlayStartOnStop(bool shouldReturn) noexcept;
    bool shouldReturnToPlayStartOnStop() const noexcept;
    void setMonitorGainLinear(float linearGain) noexcept;
    float getMonitorGainLinear() const noexcept;
    void setIncludeLfeInDownmix(bool shouldInclude) noexcept;
    bool shouldIncludeLfeInDownmix() const noexcept;
    void setMonitorMode(MonitorMode mode) noexcept;
    MonitorMode getMonitorMode() const noexcept;
    void setSourceChannelMuted(int sourceChannel, bool shouldMute) noexcept;
    bool isSourceChannelMuted(int sourceChannel) const noexcept;
    void setSourceChannelSoloed(int sourceChannel, bool shouldSolo) noexcept;
    bool isSourceChannelSoloed(int sourceChannel) const noexcept;
    void clearSourceChannelSoloMute() noexcept;
    void setOutputMapping(const ChannelOutputMapping& mapping) noexcept;
    ChannelOutputMapping getOutputMapping() const noexcept;
    void clearOutputMapping() noexcept;
    void pingSpeakerRole(SpeakerRole role) noexcept;
    float getOutputPeakLinear() const noexcept;
    float getTruePeakLinear() const noexcept;
    float getOutputRmsLinear() const noexcept;
    float getMomentaryLufs() const noexcept;
    OutputMeterSnapshot getOutputMeterSnapshot() const noexcept;
    // Control-thread admission for a visible post-Core view or explicit task.
    OutputMeterAnalysisLease acquireOutputMeterAnalysisLease();
    int getOutputMeterAnalysisLeaseCountForDiagnostics() const noexcept;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // Control-thread context for future admissions; existing tokens keep their generation.
    void setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept;
#endif
    AmbisonicFieldSnapshot getAmbisonicFieldSnapshot() const noexcept;
    AmbisonicFieldAnalysisLease acquireAmbisonicFieldAnalysisLease();
    SpatialEnergyService::Lease acquireSpatialEnergyAnalysisLease(bool postCoreReady);
    // Visible-view owner-thread poll; never initializes/enumerates a device.
    void refreshSpatialEnergyAnalysis();
    std::shared_ptr<const SpatialEnergyMeter::Snapshot> getSpatialEnergySnapshot() const { return spatialEnergy.snapshot(); }
    SpatialEnergyService::Diagnostics getSpatialEnergyDiagnostics() const { return spatialEnergy.diagnostics(); }
    bool setAmbisonicOrientationPreview(AmbisonicOrientation orientation);
    void clearAmbisonicOrientationPreview() noexcept;
    bool activateAmbisonicDecodeMatrixArtifact(
        std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact);
    void clearAmbisonicDecodeMatrixArtifact() noexcept;
    bool previewAmbisonicDecodeMatrixArtifact(
        std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact);
    void clearAmbisonicDecodeMatrixPreview() noexcept;
    void setSelectionPreview(const std::optional<juce::Range<double>>& selectionSeconds, float linearGain) noexcept;
    void clearSelectionPreview() noexcept;
    void setAudioSuitePreview(std::shared_ptr<const AudioSuitePreviewState> previewState) noexcept;
    void clearAudioSuitePreview() noexcept;
    void warmUpAudioDeviceAsync(int delayMs = 0);
    // Startup selection is application-owned. Reserve it before any ordinary
    // warmup so an unavailable saved backend cannot open a Windows fallback.
    bool reserveExplicitStartupOutput() noexcept;
    bool hasExplicitOutputSelection() const noexcept { return explicitOutputSelection.load(std::memory_order_acquire); }
    bool isSelectedOutputReady() const noexcept;
    std::optional<AsioDriverEvents::Snapshot> readManagedOutputEvents(bool consume, bool counters = false);
    std::optional<DeviceOutput::Snapshot> activeOutputSnapshot() const;
    std::shared_ptr<const DeviceRoutePlan> activeOutputPlan() const { return deviceRoutePlan.load(std::memory_order_acquire); }
    bool refreshManagedOutputLatency();

    bool hasLoadedFile() const noexcept;
    bool isPlaying() const noexcept;
    // Passive admission read for an explicitly requested diagnostic operation.
    bool isIdleForDiagnosticProbe() const noexcept;
    PlaybackState getPlaybackState() const noexcept;
    double getCurrentPositionSeconds() const noexcept;
    double getDurationSeconds() const noexcept;
    double getPlayStartSeconds() const noexcept;
    juce::Range<int64_t> getPlayableSampleRange() const noexcept;
    juce::Range<double> getPlayableTimeRange() const noexcept;
    bool canPlayAtSeconds(double seconds) const noexcept;
    LoadedFileInfo getLoadedFileInfo() const;
    // Owner-thread configuration only. A diagnostic lease never creates a
    // replacement manager; callers must show busy when this returns nullptr.
    juce::AudioDeviceManager* getDeviceManagerForConfiguration() noexcept;
    struct DeviceProbeConfiguration final
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        juce::String backend, endpoint;
        bool operator==(const DeviceProbeConfiguration&) const = default;
    };
    std::optional<DeviceProbeConfiguration> getDeviceProbeConfiguration() const;
    class DeviceLease final
    {
    public:
        ~DeviceLease() { if (identity) identity->windowOpen.store(false, std::memory_order_release); }
        DeviceLease(const DeviceLease&) = delete;
        DeviceLease& operator=(const DeviceLease&) = delete;
        const juce::AudioDeviceManager::AudioDeviceSetup& originalSetup() const noexcept { return setup; }
        const juce::String& originalBackend() const noexcept { return backend; }
        bool originallyOpen() const noexcept { return wasOpen; }
        bool ownerAlive() const noexcept { return identity->ownerAlive.load(std::memory_order_acquire); }
        // Worker-only, in order. Neither method accesses AudioEngine or UI.
        bool closeForProbe();
        bool closeForSwitch();
        bool releaseForReplacement();
        // True only after this exact live owner's old device has closed.
        bool hasExclusiveSwitchWindow() const noexcept;
        DeviceSwitchAccess exclusiveAccess() const noexcept { return DeviceSwitchAccess(identity); }
        bool restore(bool mayReopen = true);
        bool restorationSucceeded() const noexcept { return restored; }
    private:
        friend class AudioEngine;
        friend class DeviceSwitchCoordinator;
        DeviceLease() = default;
        bool closeForTransfer();
        std::unique_ptr<juce::AudioDeviceManager> manager;
        std::unique_ptr<DeviceOutput> output;
        std::shared_ptr<DeviceLeaseIdentity> identity;
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        juce::String backend, endpoint, deviceName;
        juce::BigInteger inputs, outputs;
        double rate = 0.0;
        int buffer = 0;
        bool forSwitch = false;
        bool wasOpen = false, wasReady = false, closeAttempted = false, closed = false, restoreAttempted = false, restored = false;
    };
    // Owner-thread admission/return. A lease is tied to this exact live engine.
    std::unique_ptr<DeviceLease> takeDeviceLeaseForProbe(const DeviceProbeConfiguration* expected = nullptr);
    // Exclusive D106 window after candidate static validation. Pauses the
    // existing transport; worker ownership contains no engine/source callback.
    std::unique_ptr<DeviceLease> takeDeviceLeaseForSwitch();
    bool returnDeviceLease(std::unique_ptr<DeviceLease>&);
    bool isDeviceLeasedForProbe() const noexcept { return deviceLeased.load(std::memory_order_acquire); }
    void selectAudioDeviceType(const juce::String& type);
    juce::String applyAudioDeviceSetup(const juce::AudioDeviceManager::AudioDeviceSetup&);
    int preferMaximumOutputChannelsForCurrentDevice();

    struct RuntimeMetrics
    {
        juce::String deviceName;
        juce::String backend;
        double sampleRate = 0.0, callbackLoadFraction = 0.0;
        int bufferSamples = 0, totalXruns = 0, driverXruns = -1;
        int activeOutputChannels = 0, outputLatencySamples = 0;
        bool audioSuitePreviewConfigured = false;
        PlaybackRateSource::Snapshot playbackRate;
        // Device-reported output latency plus added timeline delay in the
        // playback chain. Read-ahead and the device buffer are not added twice.
        std::optional<int> aggregateOutputLatencySamples;
        std::optional<double> aggregateOutputLatencyMs;
        int processingLatencySamples = 0, previewLookaheadSourceSamples = 0;
        std::optional<std::uint64_t> driverResets;
    };
    // Message-thread diagnostic read. Never initialises or waits for a device.
    std::optional<RuntimeMetrics> getRuntimeMetricsForDiagnostics() const;

    using DeviceRequestKind = singleeditor::DeviceRequestKind;
    using DeviceRequestOutcome = singleeditor::DeviceRequestOutcome;
    using DeviceRequestRecord = singleeditor::DeviceRequestRecord;
    struct RenderDiagnostics
    {
        enum class Availability { notRendered, busy, available, notSupported };
        Availability availability = Availability::notRendered;
        std::optional<RoutingMatrix> matrix;
        MonitorMode monitorMode = MonitorMode::autoFollowDevice;
        bool ambisonic = false, binauralRequested = false, binauralActive = false;
    };
    struct DeviceDiagnostics
    {
        enum class Availability { cold, busy, available };
        Availability availability = Availability::cold;
        std::optional<RuntimeMetrics> active;
        // Compact render slot -> zero-based physical device output. This is
        // the open device's channel selection, not speaker roles or wiring.
        std::optional<juce::Array<int>> outputChannelMapping;
        // Immutable approved route copied with the active device under the
        // same owner lock, so deferred formatting cannot observe another owner.
        std::shared_ptr<const DeviceRoutePlan> outputPlan;
        RenderDiagnostics render;
        juce::String outputEndpoint; // exact WASAPI ID; only bundle-scoped digest may be exported
        std::optional<DeviceRequestRecord> latestRequest, latestFailure;
        bool requestObservationBusy = false;
        juce::String activeOutputIdentity; // equality only, never a public device identifier
        std::optional<DeviceOutput::DriverIdentity> driverIdentity;
    };
    // One coherent passive read; no manager creation, polling or driver open.
    DeviceDiagnostics getDeviceDiagnostics() const;

    // Headless verification path that renders the same source used by playback without opening a device.
    bool renderDocumentBlockForDiagnostics(int64_t startSample, juce::AudioBuffer<float>& destination);
    bool renderTransportBlockForDiagnostics(int64_t startSample, juce::AudioBuffer<float>& destination,
                                           double outputSampleRate = 0.0);
    int getTransportChannelCapacityForDiagnostics() const noexcept;
    bool isAmbisonicBinauralRendererActive() const noexcept;
    // Control-thread maintenance. The ordinary path returns without locking;
    // retired renderers remain owned until all in-flight render leases drain.
    void collectRetiredBinauralRenderers();
#if SINGLE_EDITOR_AUDIO_RESOURCE_TESTING
    std::shared_ptr<const void> retainBinauralRendererForTesting() const;
#endif
    bool hasAmbisonicFieldAnalyzerForDiagnostics() const noexcept;
    bool hasAmbisonicStageGraphForDiagnostics() const noexcept;
    bool isCustomAmbisonicDecodeMatrixActiveForDiagnostics() const noexcept;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
        getAmbisonicDecodeMatrixArtifactForDiagnostics() const noexcept;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
        getAmbisonicDecodeMatrixPreviewForDiagnostics() const noexcept;
    int getAmbisonicFieldAnalysisLeaseCountForDiagnostics() const noexcept;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    friend struct AudioEngineDeviceTestAccess;
    friend class DeviceSwitchCoordinator;
    class DocumentAudioSource;
    class TransportRenderSource;

    bool initialiseAudioDevice();
    std::unique_ptr<DeviceLease> takeDeviceLease(bool forSwitch, const DeviceProbeConfiguration* expected);
    // Internal success seam. Check the frozen review against the coordinator's
    // current selection. On success, keep the retired lease for worker disposal.
    bool commitDeviceLease(std::unique_ptr<DeviceLease>&, std::unique_ptr<DeviceOutput>&,
        std::shared_ptr<const DeviceRoutePlan>, const DeviceRoutePlan::Bus&,
        const DeviceRoutePlan::TrustedRoles& = {});
    void rebindTransportForRoute(); // callbacks detached and transport stopped
    std::optional<RuntimeMetrics> readActiveDeviceMetrics() const; // caller holds audioDeviceLock
    void readPlaybackMetrics(RuntimeMetrics&) const;
    int enableMaximumDeviceOutputs(int limit, DeviceRequestRecord* parent = nullptr); // caller holds audioDeviceLock
    DeviceRequestRecord makeDeviceRequest(DeviceRequestKind, const juce::AudioDeviceManager::AudioDeviceSetup*) const;
    void finishDeviceRequest(DeviceRequestRecord, bool rejected, bool stopped = false);
    void ensureAudioDeviceReady();
    void setPlaybackState(PlaybackState newState);
    void updatePositionFromTransport() noexcept;
    void updateFromDocumentState(SingleTrackDocument::StatePtr state);
    void documentChanged(SingleTrackDocument::StatePtr state) override;
    void documentIdentityChanged(const media::IdentityEvent&) override;
    void publishOutputMeterSnapshot(const OutputMeterSnapshot& snapshot) noexcept;
    static double clampSeconds(double seconds, double durationSeconds) noexcept;

    SingleTrackDocument& document;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext optionalLifecycleContext = currentOptionalLifecycleContext();
#endif
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::unique_ptr<DeviceOutput> deviceOutput;
    std::atomic<std::shared_ptr<const DeviceRoutePlan>> deviceRoutePlan;
    std::shared_ptr<DeviceLeaseIdentity> deviceLeaseIdentity;
    std::weak_ptr<DeviceSwitchCoordinator> deviceSwitchOperation; // empty until explicit device intent
    std::atomic<bool> deviceLeased { false }, deviceRestoreUnavailable { false };
    // Owner-thread intent while the worker holds the actual device. User
    // commands and document replacement supersede the original resume intent.
    std::optional<bool> switchResumePlayback;
    std::shared_ptr<DeviceRequestState> deviceRequests;
    std::shared_ptr<DeviceRequestState> ensureDeviceRequestState();
    juce::AudioSourcePlayer sourcePlayer;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<DocumentAudioSource> documentSource;
    std::unique_ptr<PlaybackRateSource> playbackRateSource;
    std::shared_ptr<AmbisonicFieldLeaseControl> ambisonicFieldLeaseControl;
    std::shared_ptr<OutputMeterLeaseControl> outputMeterLeaseControl;
    std::unique_ptr<TransportRenderSource> renderSource;
    SpatialEnergyService spatialEnergy;
    juce::ListenerList<Listener> listeners;
    mutable juce::CriticalSection infoLock;
    LoadedFileInfo loadedFileInfo;
    std::atomic<double> currentPositionSeconds { 0.0 };
    std::atomic<double> durationSeconds { 0.0 };
    std::atomic<double> playStartSeconds { 0.0 };
    std::atomic<bool> loopEnabled { false };
    std::atomic<bool> returnToPlayStartOnStop { true };
    std::atomic<float> monitorGainLinear { 1.0f };
    std::atomic<bool> includeLfeInDownmix { false };
    std::atomic<int> monitorModeValue { static_cast<int>(MonitorMode::autoFollowDevice) };
    std::array<std::atomic<bool>, 32> sourceChannelMuted {};
    std::array<std::atomic<bool>, 32> sourceChannelSoloed {};
    std::atomic<bool> outputMappingEnabled { false };
    std::array<std::atomic<int>, ChannelOutputMapping::maxRoleSlots> outputChannelBySpeakerRole {};
    std::atomic<int> speakerPingRoleValue { static_cast<int>(SpeakerRole::unknown) };
    std::atomic<int> speakerPingSamplesRemaining { 0 };
    std::atomic<float> outputPeakLinear { 0.0f };
    std::atomic<float> truePeakLinear { 0.0f };
    std::atomic<float> outputRmsLinear { 0.0f };
    std::atomic<float> momentaryLufs { -120.0f };
    std::atomic<int> outputMeterChannelCount { 0 };
    std::array<std::atomic<int>, OutputMeterSnapshot::maxChannels> outputMeterRoles {};
    std::array<std::atomic<float>, OutputMeterSnapshot::maxChannels> outputMeterTruePeak {};
    std::array<std::atomic<float>, OutputMeterSnapshot::maxChannels> outputMeterRms {};
    std::array<std::atomic<float>, OutputMeterSnapshot::maxChannels> outputMeterKContributionDb {};
    std::atomic<float> outputMeterMomentaryLufs { -120.0f };
    std::atomic<bool> selectionPreviewActive { false };
    std::atomic<double> selectionPreviewStartSeconds { 0.0 };
    std::atomic<double> selectionPreviewEndSeconds { 0.0 };
    std::atomic<float> selectionPreviewGainLinear { 1.0f };
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedPreviewPublication
    {
        OptionalLifecycleToken token;
        ~ObservedPreviewPublication() { if (token.isOpen()) token.withdraw(); }
    } audioSuitePreviewPublication;
    // Declared before the real atomic publication: reverse member destruction
    // records final engine withdrawal only after its state reference is gone.
#endif
    std::atomic<std::shared_ptr<const AudioSuitePreviewState>> audioSuitePreviewState;
    std::atomic<PlaybackState> playbackState { PlaybackState::stopped };
    std::atomic<int> transportChannelCapacity { 0 };
    std::atomic<bool> audioDeviceReady { false };
    std::atomic<bool> explicitOutputSelection { false };
    juce::CriticalSection audioDeviceLock;
    AudioDeviceWarmupTask deviceWarmupTask;
};
} // namespace singleeditor
