#pragma once

#include <memory>

#include <juce_gui_extra/juce_gui_extra.h>

#include "Audio/AmbisonicSpeakerGeometry.h"
#include "Audio/AudioEngine.h"

namespace singleeditor
{
class SpeakerGeometryMagComponent final : public juce::Component
{
public:
    explicit SpeakerGeometryMagComponent(AudioEngine& engineToUse);
    ~SpeakerGeometryMagComponent() override;

    void setAmbisonicOrder(int order);
    int getAmbisonicOrder() const noexcept { return ambisonicOrder; }
    void paint(juce::Graphics& g) override;
    void resized() override;
    static int contentHeightForViewport(int width, int height, int scrollbarWidth) noexcept;
    // File completions shared by the choosers and programmatic callers.
    void importProfileFile(const juce::File& file);
    // The chooser supplies the snapshot captured when Export was opened.
    void exportProfileFile(const juce::File& file, const SpeakerGeometryProfile& profile);

private:
    void deriveSelectedTemplate();
    void rebuildSpeakerSelector();
    void loadSelectedSpeaker();
    void commitSelectedSpeaker();
    void commitProfilePolicy();
    void publishDraftMutation();
    void validateDraft();
    void generateCandidate();
    void toggleAudition();
    void activateCandidate();
    void importProfile();
    void exportProfile();
    void refreshState();
    void drawGeometryMap(juce::Graphics& g) const;
    void clearAudition() noexcept;
    juce::String stateText() const;
    juce::String diagnosticsText() const;
    static juce::String templateLabel(const juce::String& profileId);

    AudioEngine& engine;
    AmbisonicSpeakerGeometrySession session;
    SpeakerGeometryProfile draft;
    bool hasDraft = false;
    bool suppressCallbacks = false;
    bool generationInFlight = false;
    int selectedSpeakerIndex = -1;
    int ambisonicOrder = 1;
    juce::String transientStatus;

    juce::ComboBox templateBox;
    juce::TextButton deriveButton { "Derive" };
    juce::ComboBox dimensionalityBox;
    juce::ComboBox weightPolicyBox;
    juce::ComboBox speakerBox;
    juce::ComboBox roleBox;
    juce::TextEditor azimuthEditor;
    juce::TextEditor elevationEditor;
    juce::TextEditor distanceEditor;
    juce::TextEditor weightEditor;
    juce::Label azimuthLabel { {}, "Azimuth deg" };
    juce::Label elevationLabel { {}, "Elevation deg" };
    juce::Label distanceLabel { {}, "Distance m" };
    juce::Label weightLabel { {}, "Weight" };
    juce::ToggleButton enabledButton { "Enabled" };
    juce::ToggleButton availableButton { "Available" };
    juce::TextButton validateButton { "Validate" };
    juce::TextButton generateButton { "Generate" };
    juce::TextButton auditionButton { "Audition" };
    juce::TextButton activateButton { "Activate" };
    juce::TextButton importButton { "Import" };
    juce::TextButton exportButton { "Export" };
    juce::Label statusLabel;
    juce::Label identityLabel;
    juce::Label diagnosticsLabel;
    juce::Rectangle<int> geometryMapBounds;
    std::unique_ptr<juce::FileChooser> fileChooser;
};
} // namespace singleeditor
