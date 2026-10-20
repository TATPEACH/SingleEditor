#include "App/SingleEditorApplication.h"
#include "App/CoreOpenProbe.h"
#include "App/CoreOpenCycleProbe.h"
#include "App/ShutdownDiagnostics.h"
#include "DiagnosticBuildIdentity.h"

#include "UI/Components/PreferencesComponent.h"
#include "UI/MainComponent.h"
#include "UI/Renderer/RendererSelection.h"
#include "UI/LookAndFeel/ClassicLookAndFeel.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"

#include <future>

namespace singleeditor
{
SingleEditorApplication::SingleEditorApplication()
    : audioEngine(document),
      waveformModel(document)
{
}

const juce::String SingleEditorApplication::getApplicationName()
{
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    return "Single Editor Timing";
#else
    return "Single Editor";
#endif
}

const juce::String SingleEditorApplication::getApplicationVersion()
{
    return SINGLE_EDITOR_DIAGNOSTIC_VERSION;
}

bool SingleEditorApplication::moreThanOneInstanceAllowed()
{
    return false;
}

void SingleEditorApplication::initialise(const juce::String& commandLine)
{
    CoreOpenProbe::attachFromEnvironment();
    CoreOpenCycleProbe::attachFromEnvironment();
    const auto startupOpenRequestedAtMs = juce::Time::getMillisecondCounterHiRes();
#if JUCE_WINDOWS
    // Native images share JUCE's DirectX resources with the window peer. Start
    // device creation while Windows creates the HWND, and keep the reference
    // alive until the peer has acquired it. No drawing runs on this worker.
    auto graphicsWarmup = std::async(std::launch::async, []
    {
        return juce::Image(juce::Image::ARGB, 1, 1, true, juce::NativeImageType{});
    });
#endif
    configureAppProperties();
    selectRenderer(commandLine);
    PreferencesComponent::applyStoredChannelOrderDefaults(appProperties);
    PreferencesComponent::applyStoredVisualPreferences(appProperties);
    auto initialFile = parseInitialFile(commandLine);
    if (RendererSelection::selected() == RendererKind::modern)
    {
        startupWindowChrome = std::make_unique<WindowChromeLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel(startupWindowChrome.get());
    }
    else
    {
        // Classic draws its startup frame with JUCE's own LookAndFeel. Name the
        // Classic face there too: JUCE keeps text layouts made before CoreReady.
        juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName(
            ClassicLookAndFeel::sansSerifFamily);
    }
    audioEngine.warmUpAudioDeviceAsync(initialFile.existsAsFile() ? 40 : 420);

    mainWindow = std::make_unique<MainWindow>(getApplicationName(),
                                              appProperties,
                                              document,
                                              bnkDocument,
                                              playlistModel,
                                              audioEngine,
                                              waveformModel,
                                              [this] { ensureLookAndFeelReady(); },
                                              initialFile,
                                              startupOpenRequestedAtMs);
#if JUCE_WINDOWS
    const auto graphicsWarmupImage = graphicsWarmup.get();
    juce::ignoreUnused(graphicsWarmupImage);
#endif
}

void SingleEditorApplication::shutdown()
{
    traceShutdown("application.shutdown.begin");
    mainWindow.reset();
    traceShutdown("application.window.destroyed");
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    lookAndFeel.reset();
    startupWindowChrome.reset();
    CoreOpenProbe::detach();
    CoreOpenCycleProbe::detach();
    traceShutdown("application.shutdown.end");
}

void SingleEditorApplication::anotherInstanceStarted(const juce::String& commandLine)
{
    auto requestedFile = parseInitialFile(commandLine);
    if (mainWindow != nullptr)
    {
        audioEngine.warmUpAudioDeviceAsync(requestedFile.existsAsFile() ? 24 : 120);
        mainWindow->restoreFromBackground();
        if (requestedFile.existsAsFile())
            mainWindow->openFileWhenReady(requestedFile);
    }
}

void SingleEditorApplication::systemRequestedQuit()
{
    if (mainWindow != nullptr)
    {
        mainWindow->requestUserCloseAction();
        return;
    }

    quit();
}

void SingleEditorApplication::configureAppProperties()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "SingleEditor";
    options.filenameSuffix = "settings";
    options.folderName = "SingleEditor";
    options.osxLibrarySubFolder = "Application Support";
    appProperties.setStorageParameters(options);
}

juce::File SingleEditorApplication::parseInitialFile(const juce::String& commandLine) const
{
    const auto arguments = juce::StringArray::fromTokens(commandLine, true);
    for (int index = 0; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index].trim();
        if (argument == "--renderer")
        {
            ++index;
            continue;
        }
        if (argument.startsWithIgnoreCase("--renderer="))
            continue;
        if (!argument.startsWithChar('-'))
            return juce::File(argument.unquoted());
    }

    return {};
}

void SingleEditorApplication::selectRenderer(const juce::String& commandLine)
{
    auto selected = RendererKind::modern;
    if (auto* settings = appProperties.getUserSettings())
        selected = RendererSelection::parse(settings->getValue(PreferencesComponent::rendererModeKey,
                                                                 "modern").toStdString());

    const auto arguments = juce::StringArray::fromTokens(commandLine, true);
    for (int index = 0; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index].trim();
        if (argument == "--renderer" && index + 1 < arguments.size())
        {
            selected = RendererSelection::parse(arguments[++index].toStdString(), selected);
            continue;
        }
        if (argument.startsWithIgnoreCase("--renderer="))
            selected = RendererSelection::parse(argument.fromFirstOccurrenceOf("=", false, false)
                                                         .toStdString(),
                                                 selected);
    }

    RendererSelection::select(selected);
}

void SingleEditorApplication::ensureLookAndFeelReady()
{
    if (lookAndFeel == nullptr)
        lookAndFeel = createSelectedLookAndFeel();

    juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
}
} // namespace singleeditor
