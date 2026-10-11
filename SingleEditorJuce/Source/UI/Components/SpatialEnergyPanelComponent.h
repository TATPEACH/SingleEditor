#pragma once

#include "Audio/AudioEngine.h"
#include "SpatialEnergyVisualField.h"
#include "UI/Workspace/WorkspaceVisualContractTarget.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
class SpatialEnergyPanelComponent final : public juce::Component,
                                          public WorkspaceVisualContractTarget,
                                          private juce::Timer
{
public:
    enum class View { compact, standard, expanded };
    explicit SpatialEnergyPanelComponent(AudioEngine&);
    ~SpatialEnergyPanelComponent() override;
    void setCoreReady(bool);
    void applyWorkspaceVisualContract(RendererDensity, RendererMagSize, RendererInformationTier, bool) override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    juce::String getAccessibilityValueSummary() const;
    View currentView() const noexcept { return view; }
    bool hasVisualWorker() const noexcept { return worker != nullptr; }
    bool hasSphereResource() const noexcept;
    bool hasViewLease() const noexcept { return static_cast<bool>(lease); }
    bool isViewTimerRunning() const noexcept { return isTimerRunning(); }
    // Display-only inspection API also used by keyboard/UIA controls.
    void setSphereInspection(bool);

private:
    friend struct SpatialEnergyPanelTestAccess;
    class VisibilityWatcher;
    class VisualWorker;
    void refreshActivity();
    void releaseSpherePreservingPlane();
    void timerCallback() override;
    void updateReadouts();
    void rebuildImage();
    void chooseView();
    void drawMap(juce::Graphics&, juce::Rectangle<float>);
    void drawSphere(juce::Graphics&, juce::Rectangle<float>);
    static juce::Colour colour(double db, const SpatialEnergyVisualResponseProfile&);
    static juce::String number(double);

    AudioEngine& engine;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext optionalLifecycleContext = currentOptionalLifecycleContext();
    OptionalLifecycleToken timerLifecycle;
#endif
    SpatialEnergyService::Lease lease;
    std::unique_ptr<VisibilityWatcher> watcher;
    std::unique_ptr<VisualWorker> worker;
    std::shared_ptr<const SpatialEnergyMeter::Snapshot> snapshot;
    std::shared_ptr<const SpatialEnergyVisualField> field;
    SpatialEnergyVisualResponseProfile response;
    std::vector<double> displayedPowers;
    juce::Image mapImage;
    juce::ComboBox reading;
    juce::TextButton sphereButton { "3D Sphere" };
    juce::Slider floorControl, ceilingControl;
    juce::Label heading, loudness, diagnostics, warnings;
    juce::Component table;
    juce::Viewport viewport;
    std::vector<std::unique_ptr<juce::Label>> rows;
    juce::String retainedSummary;
    juce::Rectangle<float> mapBounds, sphereBounds, legendBounds;
    RendererInformationTier requestedTier = RendererInformationTier::standard;
    View view = View::compact;
    bool coreReady = false, inspectSphere = false, geometryFailure = false;
    double lastTickMs = 0, stoppedAtMs = 0, overloadUntilMs = 0, yaw = 0, pitch = 0, zoom = 1;
    juce::Point<float> dragOrigin;
    double dragYaw = 0, dragPitch = 0;
};
}
