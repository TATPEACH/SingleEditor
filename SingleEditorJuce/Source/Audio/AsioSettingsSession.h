#pragma once
#include "Audio/DeviceSwitchCoordinator.h"
#include "Audio/JuceAsioBackendAdapter.h"
#include <juce_data_structures/juce_data_structures.h>
#include <deque>
#include <map>

namespace singleeditor
{
// Owned by the application, not the settings window. All public calls are on
// the message thread; only detached backend functions cross to a worker.
class AsioSettingsSession final : public juce::ChangeBroadcaster, private AudioEngine::Listener
{
public:
    using Adapter = JuceAsioBackendAdapter;
    using Switch = DeviceSwitchCoordinator;
    enum class Activity { idle, discovery, inspection, ping, activation, maintenance, panel, followSource };
    struct Profile { juce::String name; DeviceRoutePlan::Bus bus; };
    struct Catalog : Adapter::Catalog
    {
        Catalog() = default;
        Catalog(Adapter::Catalog value) : Adapter::Catalog(std::move(value)) {}
        std::string defaultOutputId;
    };
    explicit AsioSettingsSession(AudioEngine&, juce::ApplicationProperties&);
    ~AsioSettingsSession() override;
    void refreshDrivers();
    void selectBackend(const std::string&);
    void chooseWindowsDefault();
    void selectDriver(const std::string& stableId);
    void inspect();
    void selectProfile(int index);
    void setFormat(double rate, int buffer);
    void setConnection(int row, int physical, bool connected);
    void validate();
    void ping(int physical);
    void activate(bool explicitlyConfirmWarnings);
    static bool hasSavedSelection(juce::ApplicationProperties&);
    bool canRetrySavedSelection() const { return hasSavedSelection(properties); }
    void restoreSavedSelection();
    void retryOutput();
    void openControlPanel();
    bool canMaintainOutput() const;
    bool canOpenControlPanel() const;
    bool followsSourceSampleRate() const noexcept { return followSource; }
    bool canFollowSourceSampleRate() const;
    void setFollowSourceSampleRate(bool);
    void setDeviceUiConsumer(const void* owner, std::function<bool()> isVisible = {})
    { if (isVisible) visibleConsumers[owner] = std::move(isVisible); else visibleConsumers.erase(owner); }
    const juce::String& liveOutputStatus() const noexcept { return liveStatus; }
    void cancel();
    void poll();
    void shutdown();
    bool needsPoll() const { return busy() || monitoringOutput || followPending || hasVisibleConsumer(); }
    bool busy() const noexcept { return discovery != nullptr || operation != nullptr; }
    bool driverUnresolved() const { return operation && operation->snapshot().driverUnresolved; }
    const Catalog& drivers() const noexcept { return catalog; }
    const std::string& selectedDriver() const noexcept { return selectedId; }
    const std::string& selectedBackend() const noexcept { return backendId; }
    const Adapter::PreparedCapabilities* capabilities() const noexcept { return facts.get(); }
    const std::vector<Profile>& profiles() const noexcept { return availableProfiles; }
    int selectedProfile() const noexcept { return profileIndex; }
    double sampleRate() const noexcept { return rate; }
    int bufferSize() const noexcept { return buffer; }
    const DeviceRoutePlan::Draft& draft() const noexcept { return routing; }
    const std::optional<DeviceRoutePlan::Review>& review() const noexcept { return reviewed; }
    const juce::String& status() const noexcept { return statusText; }
    Activity activity() const noexcept { return currentActivity; }
    std::uint64_t revision() const noexcept { return generation; }
    static constexpr const char* bindingKey = "asioDeviceBindingV1";
    static constexpr const char* windowsBindingKey = "windowsDeviceBindingV1";
    static constexpr const char* activeBackendKey = "activeOutputBackendV1";
    static constexpr const char* deviceBindingsKey = "confirmedDeviceBindingsV1";
    static constexpr const char* followSourceKey = "followSourceSampleRateV1";
private:
    friend struct AsioSettingsTestAccess;
    struct Backend
    {
        std::function<Catalog(const std::string&)> discover;
        Switch::Inspect inspect;
        Switch::Prepare prepare;
        Switch::PanelInspect panel;
    };
    AsioSettingsSession(AudioEngine&, juce::ApplicationProperties&, Backend);
    struct Discovery;
    static Backend defaultBackend();
    static juce::String savedBackend(juce::ApplicationProperties&);
    const char* selectedBindingKey() const noexcept { return backendId == "ASIO" ? bindingKey : windowsBindingKey; }
    void changed(const juce::String&);
    void invalidate();
    void rebuildDraft();
    void refreshProfiles();
    DeviceRoutePlan::Bus currentBus() const;
    DeviceOutput::Snapshot expected() const;
    Switch::InspectionSelection inspectionSelection() const;
    Switch::Selection selection() const;
    bool validFormat() const;
    bool persistCommitted();
    void beginMaintenance(bool panel, bool explicitRetry);
    void serviceDriverEvents();
    void loadedFileChanged(const AudioEngine::LoadedFileInfo&) override;
    void serviceFollowSource();
    void refreshLiveStatus();
    bool hasVisibleConsumer() const;
    bool followSource = false, followPending = false;
    double sourceRate = 0;
    std::uint64_t sourceRateGeneration = 1;
    Switch::Selection followSelection;
    DeviceRoutePlan::Bus activeBus(const DeviceRoutePlan::Bus& recorded) const;
    struct ResetHistory { std::deque<double> attempts; bool blocked = false, rebind = false; };
    std::map<std::string, ResetHistory> resetHistory;
    Switch::Selection maintenanceSelection;
    bool monitoringOutput = false;
    std::map<const void*, std::function<bool()>> visibleConsumers;
    double lastLiveRead = 0;
    juce::String liveStatus;
    AudioEngine* engine;
    juce::ApplicationProperties& properties;
    Backend backend;
    std::shared_ptr<Discovery> discovery;
    std::shared_ptr<Switch> operation;
    Catalog catalog;
    std::shared_ptr<const Adapter::PreparedCapabilities> facts;
    std::vector<Profile> availableProfiles;
    std::string selectedId;
    std::string backendId {"ASIO"};
    int profileIndex = -1;
    double rate = 0;
    int buffer = 0;
    DeviceRoutePlan::Draft routing;
    std::optional<DeviceRoutePlan::Review> reviewed;
    std::shared_ptr<const DeviceRoutePlan> approved;
    std::uint64_t generation = 1;
    Activity currentActivity = Activity::idle;
    juce::String statusText { "Choose a backend and output, then read its capabilities." };
    Switch::Phase lastPhase = Switch::Phase::inactive;
    bool restoringSavedSelection = false;
    bool selectingWindowsDefault = false;
};
}
