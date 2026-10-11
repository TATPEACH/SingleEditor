#pragma once
#include "Diagnostics/LiveDiagnosticProviders.h"
#include "Diagnostics/CodecDiagnosticProbe.h"
#include "Diagnostics/GuiDeviceProbe.h"
#include <juce_gui_extra/juce_gui_extra.h>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor
{
struct DiagnosticDeviceActions final
{
    std::function<std::optional<AudioEngine::DeviceProbeConfiguration>()> current;
    std::function<std::shared_ptr<diagnostics::GuiDeviceProbe>(const AudioEngine::DeviceProbeConfiguration&,
        const diagnostics::DeviceProbeRequest&)> start;
};
class LocalDiagnosticsComponent final : public juce::Component, private juce::Timer
{
public:
    using Capture = std::function<std::vector<diagnostics::CapturedProvider>()>;
    explicit LocalDiagnosticsComponent(Capture, diagnostics::ProviderScope = {}, std::vector<juce::File> exportExcludedDirectories = {},
                                       std::function<bool()> probeAllowed = {}, DiagnosticDeviceActions = {});
    ~LocalDiagnosticsComponent() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    // A repeated explicit entry requests fresh owners after any approved save
    // or current collection settles; never cancel an in-progress publication.
    void requestRefresh();
    void requestScope(diagnostics::ProviderScope);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void setOptionalLifecycleContextProvider(std::function<OptionalLifecycleContext()> provider)
    { optionalLifecycleContextProvider = std::move(provider); }
#endif
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    struct ResourceObservation
    {
        std::weak_ptr<const void> job, snapshot;
        bool timerRunning = false, choosingFile = false;
    };
    // Non-owning native-test observation; absent from shipping builds.
    ResourceObservation resourcesForTesting() const;
#endif
private:
    struct Job;
    void refresh();
    void chooseDestination();
    void chooseReproduction();
    void chooseProbe();
    void startProbe(const juce::File&);
    void chooseDeviceProbe();
    void startDeviceProbe(const AudioEngine::DeviceProbeConfiguration&, const diagnostics::DeviceProbeRequest&);
    void publish(const juce::File&, std::shared_ptr<const diagnostics::PreparedSnapshot>);
    void timerCallback() override;
    void busy(bool);
    Capture capture;
    const std::function<bool()> probeAllowed;
    const DiagnosticDeviceActions deviceActions;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::function<OptionalLifecycleContext()> optionalLifecycleContextProvider;
#endif
    std::optional<diagnostics::CapturedProvider> probeObservation;
    std::optional<diagnostics::FailureCapsule> latestProbeFailure;
    std::optional<diagnostics::FailureCapsule> latestDeviceProbeFailure;
    diagnostics::ProviderScope scope;
    const std::vector<juce::File> exportExcludedDirectories;
    std::optional<diagnostics::ProviderScope> queuedScope;
    std::set<diagnostics::ProviderKind> availableProviders;
    juce::Label heading, status, reproductionStatus;
    juce::ToggleButton includePath { "Include file path" }, includeName { "Include file name" }, includeDevice { "Include device name" };
    juce::ToggleButton audioSection { "Audio device" }, spatialSection { "Spatial" }, mediaSection { "Media" },
        cacheSection { "Cache" }, recoverySection { "Recovery" };
    std::array<std::pair<diagnostics::ProviderKind, juce::ToggleButton*>, 5> sectionButtons {{
        { diagnostics::ProviderKind::audio, &audioSection }, { diagnostics::ProviderKind::spatial, &spatialSection },
        { diagnostics::ProviderKind::codec, &mediaSection }, { diagnostics::ProviderKind::cache, &cacheSection },
        { diagnostics::ProviderKind::recovery, &recoverySection } }};
    juce::TextEditor preview;
    juce::TextButton refreshButton { "Refresh snapshot" }, saveButton { "Save reviewed bundle..." };
    juce::TextButton reproductionButton { "Choose reproduction files..." }, removeReproductionButton { "Remove files" };
    juce::TextButton probeButton { "Probe audio..." }, deviceProbeButton { "Probe device..." },
        abortProbeButton { "Abort probe" }, currentStateButton { "Current state" };
    std::vector<juce::File> selectedReproductionFiles;
    std::shared_ptr<Job> job;
    std::shared_ptr<const diagnostics::PreparedSnapshot> prepared;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> deviceOptions;
    juce::ScopedMessageBox reproductionConfirmation, probeConfirmation;
    bool refreshQueued = false, confirmationPending = false, shuttingDown = false, currentStateQueued = false;
};
}
