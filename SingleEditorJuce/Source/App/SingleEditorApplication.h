#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "App/MainWindow.h"
#include "Audio/AudioEngine.h"
#include "Bnk/BnkDocument.h"
#include "Document/PlaylistModel.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Renderer/RendererLookAndFeelFactory.h"
#include "Waveform/WaveformModel.h"

namespace singleeditor
{
class SingleEditorApplication final : public juce::JUCEApplication
{
public:
    SingleEditorApplication();

    const juce::String getApplicationName() override;
    const juce::String getApplicationVersion() override;
    bool moreThanOneInstanceAllowed() override;

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;
    void anotherInstanceStarted(const juce::String& commandLine) override;
    void systemRequestedQuit() override;

private:
    void configureAppProperties();
    juce::File parseInitialFile(const juce::String& commandLine) const;
    void selectRenderer(const juce::String& commandLine);
    void ensureLookAndFeelReady();

    std::unique_ptr<juce::LookAndFeel_V4> lookAndFeel;
    std::unique_ptr<juce::LookAndFeel_V4> startupWindowChrome;
    juce::ApplicationProperties appProperties;
    SingleTrackDocument document;
    BnkDocument bnkDocument;
    PlaylistModel playlistModel;
    AudioEngine audioEngine;
    WaveformModel waveformModel;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace singleeditor
