#include "App/MainWindow.h"
#include "App/WindowShellResources.h"
#include "Audio/AudioEngine.h"
#include "Audio/AmbisonicBinauralFilterBank.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "Audio/VgmstreamDecodeService.h"
#include "Bnk/BnkDocument.h"
#include "Bnk/BnkParser.h"
#include "Core/NativeFilePath.h"
#include "Document/PlaylistModel.h"
#include "Document/RecoveredPlaylistDocument.h"
#include "Recovery/RecoveryStore.h"
#include "Recovery/RecoveryService.h"
#include "Recovery/RecoveryAttemptStore.h"
#include "Recovery/RecoveryCheckpointCodec.h"
#include "UI/MainComponent.h"
#include "UI/Components/PreferencesComponent.h"
#include "Diagnostics/DiagnosticBundle.h"
#include "Diagnostics/DiagnosticTestActivity.h"
#include "Diagnostics/LiveDiagnosticProviders.h"
#include "UI/Components/LocalDiagnosticsComponent.h"
#include "UI/Components/RecoveryCenterComponent.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"
#include "UI/Renderer/RendererLookAndFeelFactory.h"
#include "DecoderProcessWitness.h"
#include "OptionalLifecycleTestSupport.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <juce_cryptography/juce_cryptography.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winioctl.h>
#endif

int runPackedDecoderFixture(int argc, char* argv[]);

namespace singleeditor
{
std::weak_ptr<const void> observeNativeBnkWorker(const MainComponent&);
bool nativeBnkWorkerFinished(const MainComponent&);
media::OptionalMediaOperation::DiagnosticHandle observeNativeBnkDiagnostic(const MainComponent&);
int runWaveformPresentationTests(const juce::File& artifacts);
int runDeferredSettingsPersistenceTests(const juce::File& artifacts);
int runDeferredWorkspaceControlsTests(const juce::File& artifacts);
int runDeferredCacheWarmTests(const juce::File& artifacts);
int runOutputMeterConsumerTests(const juce::File& artifacts);
int runLifecycleViolationTests(const juce::File& artifacts);
void runAudioDeviceDiagnosticsTests(juce::ApplicationProperties&, const juce::File&);
class DeviceBackendLifecycleTestSession;
std::shared_ptr<DeviceBackendLifecycleTestSession> runDeviceBackendLifecycleTests(const juce::File&);
void finishDeviceBackendLifecycleTests(DeviceBackendLifecycleTestSession&);
void runSpatialEnergyNativeTests(const juce::File&);
void verifyActualNativeDeviceMetadata(AudioEngine&, const juce::var&, const juce::File&);
class OptionalLifecycleTestSession;
std::shared_ptr<OptionalLifecycleTestSession> createOptionalLifecycleTests(const juce::File&, juce::ApplicationProperties&);
bool advanceOptionalLifecycleTests(OptionalLifecycleTestSession&, MainComponent&, AudioEngine&);
void finishOptionalLifecycleTests(OptionalLifecycleTestSession&, bool);
class MagLifecycleTestSession;
std::shared_ptr<MagLifecycleTestSession> createMagLifecycleTests(const juce::File&, bool);
bool advanceMagLifecycleTests(MagLifecycleTestSession&, MainComponent&, AudioEngine&);
void finishMagLifecycleTests(MagLifecycleTestSession&, bool);
class BackendLifecycleTestSession;
std::shared_ptr<BackendLifecycleTestSession> createBackendLifecycleTests(const juce::File&, juce::String);
OptionalLifecycleContext backendLifecycleContext(BackendLifecycleTestSession&, MainComponent&);
void sampleBackendLifecycleTests(BackendLifecycleTestSession&, MainComponent&);
void finishBackendLifecycleTests(BackendLifecycleTestSession&, bool);
// Actual native components, commands, Save coordinator, workers and dialogs.
// Fixtures/settings belong to this child process. No device playback/selection,
// physical-input acceptance, or CoreReady timing result is claimed by this test.
// The optional-lifecycle scenario invokes actual Preview/device initialization;
// it makes lifecycle assertions, not physical playback or performance claims.
class NativeRecoveryTestApplication final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "Single Editor Recovery Test"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}
    void initialise(const juce::String& commandLine) override
    {
        try
        {
            auto args = juce::StringArray::fromTokens(commandLine, true);
            if (args.size() == 3 && args[1] == "real-bank")
            {
                realBankInput = juce::File(args[2].unquoted());
                check(realBankInput.isDirectory(), "explicit real-bank input exists");
                args.remove(2); args.remove(1);
            }
            if (args.size() >= 2 && args[args.size() - 1] == "--observe-backends")
            {
                args.remove(args.size() - 1);
                check(args.size() == 1 || (args.size() == 2
                    && (args[1] == "packed-close" || args[1] == "diagnostics-device" || args[1] == "diagnostics-probe-real")),
                    "backend observation reuses only the selected existing native workflows");
                backendLifecycleScenario = args.size() == 1 ? juce::String("recovery-save") : args[1];
            }
            if (args.size() == 2 && (args[1] == "mag-lifecycle" || args[1] == "mag-transitions"))
            { magTransitionsMode = args[1] == "mag-transitions"; magLifecycleMode = true; args.remove(1); }
            if (args.size() == 2 && args[1] == "optional-lifecycle")
            { optionalLifecycleMode = true; args.remove(1); }
            if (args.size() == 2 && args[1] == "lifecycle-violations")
            {
                standalonePresentation = true;
                setApplicationReturnValue(runLifecycleViolationTests(juce::File(args[0].unquoted())));
                quit(); return;
            }
            if (args.size() == 2 && (args[1] == "restore-save" || args[1] == "restore-compacted-save"))
            { compactedRestoreMode = args[1] == "restore-compacted-save"; restoreSaveMode = true; args.set(1, "restore"); }
            if (args.size() == 2 && args[1] == "capacity")
            { capacityMode = true; args.remove(1); }
            if (args.size() == 2 && args[1] == "storage")
            { storageMode = true; args.set(1, "center"); }
            if (args.size() == 2 && args[1] == "retention")
            { retentionMode = true; storageMode = true; args.set(1, "center"); }
            if (args.size() == 2 && args[1].startsWith("cleanup-"))
            {
                check(juce::StringArray { "cleanup-marker", "cleanup-intent", "cleanup-auto-marker", "cleanup-auto-intent" }.contains(args[1]), "known persisted cleanup scenario");
                cleanupScenario = args[1]; args.set(1, "center");
            }
            if (args.size() == 2 && args[1] == "output-meter-consumers")
            {
                standalonePresentation = true;
                setApplicationReturnValue(runOutputMeterConsumerTests(juce::File(args[0].unquoted())));
                quit(); return;
            }
            if (args.size() == 2 && args[1] == "deferred-cache-warm")
            {
                standalonePresentation = true;
                setApplicationReturnValue(runDeferredCacheWarmTests(juce::File(args[0].unquoted())));
                quit(); return;
            }
            if (args.size() == 2 && args[1] == "deferred-workspace-controls")
            {
                standalonePresentation = true;
                setApplicationReturnValue(runDeferredWorkspaceControlsTests(juce::File(args[0].unquoted())));
                quit(); return;
            }
            if (args.size() == 2 && args[1] == "deferred-settings")
            {
                standalonePresentation = true;
                setApplicationReturnValue(runDeferredSettingsPersistenceTests(juce::File(args[0].unquoted())));
                quit(); return;
            }
            if (args.size() == 2 && args[1] == "waveform-presentation")
            {
                standalonePresentation = true;
                setApplicationReturnValue(runWaveformPresentationTests(juce::File(args[0].unquoted())));
                quit(); return;
            }
            if (args.size() == 2 && args[1].startsWith("hrtf-"))
            {
                check(juce::StringArray { "hrtf-normal", "hrtf-missing", "hrtf-corrupt", "hrtf-stale", "hrtf-close" }.contains(args[1]),
                    "known native HRTF owner resource scenario");
                hrtfScenario = args[1]; args.remove(1);
            }
            if (args.size() == 2 && args[1].startsWith("tray-"))
            {
                check(juce::StringArray { "tray-normal", "tray-precore", "tray-empty", "tray-missing", "tray-corrupt", "tray-cancel" }.contains(args[1]),
                    "known native tray resource scenario");
                trayScenario = args[1];
                args.remove(1); // Reuse ordinary fixture/settings setup; dedicated timer route below.
            }
            if (!args.isEmpty() && args[0] == "--diagnostic-codec-worker")
            {
                decoderChild = true;
                auto workerArgs = args; for (int i = 0; i < workerArgs.size(); ++i) workerArgs.set(i, workerArgs[i].unquoted());
                if (workerArgs.size() == 5 && juce::File(workerArgs[1]).getFileName() == "slow-probe.wav")
                {
                    const auto input = juce::File(workerArgs[1]);
                    check(input.getParentDirectory().getFileName().startsWith("native-recovery-"), "stalled probe child belongs to this test fixture");
                    check(input.getSiblingFile("probe-worker-live.txt").replaceWithText(tests::currentDecoderProcessWitness()), "record actual stalled worker identity");
                    juce::Thread::sleep(30000); // owned stalled-reader fixture; the real parent must cancel its process lease
                }
                const auto result = diagnostics::CodecDiagnosticProbe::tryWorker(workerArgs);
                setApplicationReturnValue(result ? *result : 2); quit(); return;
            }
            if ((args.size() == 6 && args[0] == "-i" && args[1] == "-W")
                || (args.size() == 4 && args[0] == "-i" && args[1] == "-o"))
            {
                decoderChild = true;
                juce::StringArray values { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName() };
                for (const auto& value : args) values.add(value.unquoted());
                std::vector<char*> pointers;
                for (const auto& value : values) pointers.push_back(const_cast<char*>(value.toRawUTF8()));
                setApplicationReturnValue(runPackedDecoderFixture(static_cast<int>(pointers.size()), pointers.data())); quit(); return;
            }
            if (args.contains("-hide_banner"))
            {
                decoderChild = true;
                const auto input = juce::File(args[args.indexOf("-i") + 1].unquoted());
                const auto output = juce::File(args[args.size() - 1].unquoted());
                check(input.getParentDirectory().getFileName().startsWith("native-recovery-"), "decoder child owns only private fixture");
                check(input.getParentDirectory().getChildFile("video-invocations.txt").appendText(
                    juce::String(static_cast<juce::int64>(GetCurrentProcessId())) + "\n"), "record actual decoder child PID");
                if (input.getFileName().startsWith("hang")) { juce::Thread::sleep(600000); }
                const auto quotaMarker = input.getSiblingFile("quota-first.txt");
                if (input.getFileName().startsWith("quota") && !quotaMarker.existsAsFile())
                {
                    check(quotaMarker.replaceWithText("first actual output quota attempt"), "record first quota attempt");
#if JUCE_WINDOWS
                    auto handle = CreateFileW(output.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    check(handle != INVALID_HANDLE_VALUE, "create actual sparse private staging output");
                    DWORD returned = 0;
                    const auto sparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
                    LARGE_INTEGER size; size.QuadPart = 1024LL * 1024 * 1024 + 4096;
                    const auto positioned = SetFilePointerEx(handle, size, nullptr, FILE_BEGIN);
                    const auto ended = SetEndOfFile(handle);
                    CloseHandle(handle);
                    check(sparse && positioned && ended, "real sparse logical output crosses conservative 1 GiB quota without allocating audio RAM or disk blocks");
#endif
                    setApplicationReturnValue(0); quit(); return;
                }
                const auto ok = (input.getFileName().startsWith("success") || input.getFileName().startsWith("quota"))
                    && input.getSiblingFile("native-recovery-source.wav").copyFileTo(output);
                setApplicationReturnValue(ok ? 0 : 37); quit(); return;
            }
check((args.size() == 2 && args[1] == "session-diagnostics") || (args.size() == 2 && juce::StringArray { "diagnostics-spatial", "diagnostics-video-failure", "diagnostics-video-cache", "diagnostics-packed-cache", "diagnostics-probe-real", "diagnostics-probe", "diagnostics-reproduction", "diagnostics-device", "diagnostics", "diagnostics-empty", "bank-open-derived", "bank-media-locators", "bank-state-scope", "bank-rtpc-metadata", "bank-open-core", "bank-open-sidecar", "bank-open-cancel", "bank-open-edited", "bank-open-close", "bank-open-queue", "bank-fallback-core", "bank-fallback-close", "bank-fallback-recovered", "bank-fallback-superseded" }.contains(args[1])) || args.size() == 1 || (args.size() == 2 && (args[1] == "invalid-root" || args[1] == "center" || args[1] == "empty-center" || args[1] == "restore" || args[1] == "inspect" || args[1] == "earlier" || args[1] == "video" || args[1] == "video-budget" || args[1] == "video-startup" || args[1] == "packed" || args[1] == "packed-budget" || args[1] == "packed-startup" || args[1] == "packed-close" || args[1] == "packed-recovered" || args[1] == "packed-edited" || args[1] == "packed-bank" || args[1] == "bank" || args[1] == "bank-budget" || args[1] == "bank-cancel" || args[1] == "bank-edited" || args[1] == "bank-queue" || args[1] == "bank-event" || args[1] == "bank-switch" || args[1] == "bank-replaced" || args[1] == "bank-close-cancel" || args[1] == "bank-close")),
                "provide a new artifact directory and optional invalid-root, center, empty-center or restore scenario");
            spatialDiagnosticsMode = args.size() == 2 && args[1] == "diagnostics-spatial";
            emptyCenter = trayScenario == "tray-empty" || (args.size() == 2 && args[1] == "empty-center");
            diagnosticOptionalScenario = args.size() == 2 && juce::StringArray { "diagnostics-video-failure", "diagnostics-video-cache", "diagnostics-packed-cache" }.contains(args[1]) ? args[1] : juce::String{};
            reproductionDiagnosticsMode = args.size() == 2 && args[1] == "diagnostics-reproduction";
            realProbeCompanion = args.size() == 2 && args[1] == "diagnostics-probe-real";
            guiDeviceProbeMode = args.size() == 2 && args[1] == "diagnostics-device";
            activeProbeMode = realProbeCompanion || (args.size() == 2 && args[1] == "diagnostics-probe");
            deviceDiagnosticsMode = spatialDiagnosticsMode || activeProbeMode || reproductionDiagnosticsMode || (args.size() == 2 && args[1] == "diagnostics-device");
            sessionDiagnosticsMode = args.size() == 2 && args[1] == "session-diagnostics";
            diagnosticsMode = deviceDiagnosticsMode || diagnosticOptionalScenario.isNotEmpty() || (args.size() == 2 && (args[1] == "diagnostics" || args[1] == "diagnostics-empty"));
            if (diagnosticsMode && args[1] == "diagnostics-empty") emptyCenter = true;
            earlierMode = args.size() == 2 && args[1] == "earlier";
            inspectionMode = earlierMode || (args.size() == 2 && args[1] == "inspect");
            packedRecoveredMode = args.size() == 2 && args[1] == "packed-recovered";
            packedEditedMode = args.size() == 2 && args[1] == "packed-edited";
            packedBankMode = args.size() == 2 && args[1] == "packed-bank";
            bankScenario = args.size() == 2 && args[1].startsWith("bank") ? args[1] : juce::String{};
            restoreMode = restoreSaveMode || bankScenario == "bank-fallback-recovered" || packedRecoveredMode || inspectionMode || (args.size() == 2 && args[1] == "restore");
            centerMode = restoreMode || emptyCenter || (args.size() == 2 && args[1] == "center");
            invalidRoot = args.size() == 2 && args[1] == "invalid-root";
            videoBudgetMode = args.size() == 2 && args[1] == "video-budget";
            videoStartupMode = args.size() == 2 && args[1] == "video-startup";
            videoMode = diagnosticOptionalScenario.startsWith("diagnostics-video-") || videoBudgetMode || videoStartupMode || (args.size() == 2 && args[1] == "video");
            packedBudgetMode = args.size() == 2 && args[1] == "packed-budget";
            packedStartupMode = args.size() == 2 && args[1] == "packed-startup";
            packedCloseMode = args.size() == 2 && args[1] == "packed-close";
            packedMode = packedBudgetMode || packedStartupMode || packedCloseMode || packedRecoveredMode || packedEditedMode || packedBankMode || (args.size() == 2 && args[1] == "packed");
            artifacts = juce::File(args[0].unquoted());
            packedMode = packedMode || diagnosticOptionalScenario == "diagnostics-packed-cache";
            check(!artifacts.exists() && artifacts.createDirectory().wasOk(), "create independent artifact directory without overwriting a candidate");
            parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
            fixture = parent.getChildFile("native-recovery-" + juce::Uuid().toString());
            check(fixture.createDirectory().wasOk(), "create owned native fixture");
            std::cout << "Native fixture: " << fixture.getFullPathName() << '\n';
            if (invalidRoot)
            {
                originalWorkingDirectory = juce::File::getCurrentWorkingDirectory();
                check(fixture.setAsCurrentWorkingDirectory(), "contain any erroneous relative root inside the private fixture");
            }
            const auto local = fixture.getChildFile("LocalAppData");
            check(local.createDirectory().wasOk(), "create isolated child-process local data directory");
#if JUCE_WINDOWS
            const juce::String processRoot = invalidRoot ? juce::String{} : local.getFullPathName();
            check(_wputenv_s(L"LOCALAPPDATA", processRoot.toWideCharPointer()) == 0, "redirect only this test process's recovery root");
            check(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {}) == processRoot,
                "verify the exact native environment lookup before any product service can start");
#endif
            recoveryRoot = local.getChildFile("SingleEditor/Recovery");
            journal = recoveryRoot.getChildFile("journals");
            source = fixture.getChildFile("native-recovery-source.wav");
            if (capacityMode)
                check(artifacts.getParentDirectory().getChildFile("source-10min.wav").copyFileTo(source),
                    "copy the private generated 10 minute 7.1 pilot into the owned native fixture");
            else makeSource();
            if (realBankInput != juce::File()) prepareRealBank();
            if (bankScenario.isNotEmpty())
            {
                for (int id = 100; id <= 102; ++id)
                    check(fixture.getChildFile(juce::String(id) + ".wem").replaceWithText(bankScenario == "bank-event" || (id == 100 && bankScenario == "bank")
                        ? "packed-fixture:late-exit" : id == 100 && bankScenario == "bank-budget" ? "packed-fixture:quota" : "packed-fixture:success"),
                        "create private actual BNK streamed decoder input");
                check(_wputenv_s(L"SINGLE_EDITOR_VGMSTREAM_PATH", juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                    .getFullPathName().toWideCharPointer()) == 0, "select only this native bank process decoder");
            }
            if (packedMode)
            {
                videoSource = fixture.getChildFile(diagnosticsMode ? "success-diagnostics.wem" : packedBudgetMode ? "quota.wem" : "late-exit.wem");
                check(videoSource.replaceWithText(diagnosticsMode ? "packed-fixture:success" : packedBudgetMode ? "packed-fixture:quota" : "packed-fixture:late-exit"), "create actual native packed fixture");
                if (packedStartupMode || diagnosticsMode) check(videoSource.getSiblingFile(videoSource.getFileName() + ".finish").replaceWithText("finish"), "fixture completes without a harness gate");
                check(_wputenv_s(L"SINGLE_EDITOR_VGMSTREAM_PATH", juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                    .getFullPathName().toWideCharPointer()) == 0, "select only this test-process packed decoder");
            }
            if (videoMode)
            {
                videoSource = fixture.getChildFile(diagnosticOptionalScenario == "diagnostics-video-cache" ? "success-diagnostics.mp4" : videoBudgetMode ? "quota.mp4" : "failure.mp4");
                check(videoSource.replaceWithText("private invalid video fixture"), "create actual invalid video source");
#if JUCE_WINDOWS
                check(_wputenv_s(L"SINGLE_EDITOR_FFMPEG_PATH", juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                    .getFullPathName().toWideCharPointer()) == 0, "select this child-process test decoder only");
#endif
            }
            if (centerMode && trayScenario.isEmpty()) seedRecoveryRecord();
            juce::PropertiesFile::Options options;
            options.applicationName = "NativeRecoveryTest"; options.filenameSuffix = "settings";
            options.folderName = fixture.getChildFile("settings").getFullPathName();
            properties.setStorageParameters(options);
            check(properties.getUserSettings()->getFile().isAChildOf(fixture), "all native settings remain inside the owned fixture");
            properties.getUserSettings()->setValue("autoPlayEnabled", false);
            RendererSelection::select(RendererKind::modern);
            PreferencesComponent::applyStoredChannelOrderDefaults(properties);
            PreferencesComponent::applyStoredVisualPreferences(properties);
            chrome = std::make_unique<WindowChromeLookAndFeel>();
            juce::LookAndFeel::setDefaultLookAndFeel(chrome.get());
            if (optionalLifecycleMode)
                optionalLifecycle = createOptionalLifecycleTests(artifacts, properties);
            if (magLifecycleMode)
                magLifecycle = createMagLifecycleTests(artifacts, magTransitionsMode);
            if (backendLifecycleScenario.isNotEmpty())
                backendLifecycle = createBackendLifecycleTests(artifacts, backendLifecycleScenario);
            document = std::make_unique<SingleTrackDocument>();
            bank = std::make_unique<BnkDocument>(); playlist = std::make_unique<PlaylistModel>();
            engine = std::make_unique<AudioEngine>(*document); waveform = std::make_unique<WaveformModel>(*document);
            window = std::make_unique<MainWindow>(getApplicationName(), properties, *document, *bank, *playlist, *engine, *waveform,
                [this]
                {
                    if (hrtfScenario == "hrtf-normal")
                    {
                        const auto* client = window ? window->getMainComponent() : nullptr;
                        hrtfStartupRendererPreparationObserved = true;
                        hrtfStartupRendererPreparedAfterCore = client != nullptr
                            && client->isCoreReadyForWorkspaceResources();
                        std::cout << "startup renderer preparation: core-ready="
                                  << hrtfStartupRendererPreparedAfterCore << '\n';
                    }
                    if (!look) look = createSelectedLookAndFeel();
                    juce::LookAndFeel::setDefaultLookAndFeel(look.get());
                }, emptyCenter ? juce::File() : (videoStartupMode || packedStartupMode) ? videoSource : source, juce::Time::getMillisecondCounterHiRes());
            if (realBankInput != juce::File())
            {
                check(BnkRuntimeSession::factoryRequestsForTesting() == 0 && BnkRuntimeSession::liveInstancesForTesting() == 0,
                    "native ordinary startup constructs no BNK runtime or factory request");
                realBankChecks.add("ordinary-startup-zero-bnk-factory-and-instance");
            }
            if (trayScenario.isNotEmpty())
            {
                properties.getUserSettings()->setValue(PreferencesComponent::closeToTrayOnCloseKey, true);
                check(windowShellResources::packReadAttempts == 0 && windowShellResources::imageDecodes == 0,
                    "native startup performs no tray pack read or image decode");
                if (trayScenario == "tray-precore" || trayScenario == "tray-empty")
                {
                    window->requestUserCloseAction();
                    check(window->hasNativeTrayIconForTesting() && !window->isShowing()
                        && window->trayResourceStatusForTesting() == "waiting-for-core",
                        "immediate close before CoreReady retains a usable native fallback tray");
                }
            }
            next(0); startTimer(20);
        }
        catch (const std::exception& e) { fail(e.what()); }
    }
    void shutdown() override
    {
        if (decoderChild || standalonePresentation) return;
        stopTimer(); quotaBlock.reset();
        if (packedCloseMode && !failed)
        {
            try { checkPackedOriginal(false); checkVideoChildrenExited(); }
            catch (const std::exception& e) { failed = true; std::cerr << "Packed close: " << e.what() << '\n'; }
        }
        if (bankScenario == "bank-close" && !failed)
        {
            try { checkBankWorkspace(); checkVideoChildrenExited(); checkBankInputReleased(); }
            catch (const std::exception& e) { failed = true; std::cerr << "Bank close: " << e.what() << '\n'; }
        }
        if (!expectedQuit && !failed) { failed = true; std::cerr << "FAILED: native close exited before the test allowed it\n"; }
        if (realBankInput != juce::File() && !failed)
        {
            try
            {
                const auto current = document->captureWorkspaceSnapshot();
                check(realBankCloseWorkspace ? current && current->sameContents(*realBankCloseWorkspace)
                    : realBankCloseEmptyState && document->getState() == realBankCloseEmptyState && !document->hasAudio(),
                    "actual close refuses the completed but unconsumed real-bank result");
                checkRealBankDiagnostic(true);
                realBankChecks.add("close-fences-completed-real-result");
            }
            catch (const std::exception& e) { failed = true; std::cerr << "Real bank close: " << e.what() << '\n'; }
        }
        if (!failed && bankScenario == "bank-open-close")
        {
            try { checkBankWorkspace(); checkBankOpenInputsReleased(); check(!bank->hasBank(), "close prevents all late bank publication"); }
            catch (const std::exception& e) { failed = true; std::cerr << "Bank open close: " << e.what() << '\n'; }
        }
        if (!failed && bankScenario == "bank-fallback-close")
        {
            try { checkFallbackReturn(false); checkBankWorkspace(); checkBankOpenInputsReleased(); check(!bank->hasBank(), "ordinary close fences late bank classification/publication"); }
            catch (const std::exception& e) { failed = true; std::cerr << "Bank fallback close: " << e.what() << '\n'; }
        }
        if (backendLifecycle && window && window->getMainComponent())
        {
            try { sampleBackendLifecycleTests(*backendLifecycle, *window->getMainComponent()); }
            catch (const std::exception& e) { failed = true; std::cerr << "Backend lifecycle sample: " << e.what() << '\n'; }
        }
        window.reset(); centerImage.reset(); waveform.reset(); engine.reset(); playlist.reset(); bank.reset(); document.reset();
        realBankCloseWorkspace.reset(); realBankCloseEmptyState.reset(); realBankWorkspace.reset();
        if (realBankInput != juce::File() && !failed)
        {
            try
            {
                check(realBankWorker.expired(), "actual native close releases the real-bank worker");
                check(BnkRuntimeSession::liveInstancesForTesting() == 0, "native close releases every admitted BNK session");
                realBankChecks.add("native-close-zero-bnk-instance");
                cleanupRealBank();
            }
            catch (const std::exception& e) { failed = true; std::cerr << "Real bank cleanup: " << e.what() << '\n'; }
        }
        if (optionalLifecycle)
        {
            try { finishOptionalLifecycleTests(*optionalLifecycle, optionalLifecycleCompleted && !failed); }
            catch (const std::exception& e)
            { failed = true; std::cerr << "Optional lifecycle: " << e.what() << '\n'; }
            optionalLifecycle.reset();
        }
        if (magLifecycle)
        {
            try { finishMagLifecycleTests(*magLifecycle, magLifecycleCompleted && !failed); }
            catch (const std::exception& e)
            { failed = true; std::cerr << "Mag lifecycle: " << e.what() << '\n'; }
            magLifecycle.reset();
        }
        packedEditedState.reset(); videoOriginalState.reset(); bankWorkspace.reset();
        hrtfState.reset();
        fallbackHistory.reset(); sourceRetryHistory.reset(); fallbackRedoPcm.clear(); pendingCloseWorkspace.reset();
        if (deviceBackendLifecycle)
        {
            try { finishDeviceBackendLifecycleTests(*deviceBackendLifecycle); }
            catch (const std::exception& e) { failed = true; std::cerr << "Device backend lifecycle: " << e.what() << '\n'; }
            deviceBackendLifecycle.reset();
        }
        if (backendLifecycle)
        {
            // Observe after the existing test-owned product snapshots are also
            // released, while a failed fixture can still be retained intact.
            try { finishBackendLifecycleTests(*backendLifecycle, expectedQuit && !failed); }
            catch (const std::exception& e) { failed = true; std::cerr << "Backend lifecycle: " << e.what() << '\n'; }
            backendLifecycle.reset();
        }
        if (packedBankCache != juce::File())
        {
            if (packedBankFile.getParentDirectory() != fixture || packedBankCache.getFileName() != "99.wem"
                || juce::SHA256(packedBankCache).toHexString() != packedBankCacheHash || !packedBankCache.deleteFile())
            { failed = true; std::cerr << "Changed private bank extraction retained\n"; }
        }
        if ((packedMode || bankScenario.isNotEmpty()) && fixture.isDirectory())
        {
            for (const auto& input : fixture.findChildFiles(juce::File::findFiles, false, "*.wem"))
            {
                const auto wave = VgmstreamDecodeService::getDecodedWaveFile(input);
                const auto path = wave.getFullPathName().toLowerCase();
                juce::InterProcessLock lock("SingleEditor-vgmstream-" + juce::SHA256(path.toRawUTF8(), path.getNumBytesAsUTF8()).toHexString());
                if (!lock.enter(5000) || wave.getParentDirectory() != VgmstreamDecodeService::getDecodeCacheDirectory()
                    || !wave.deleteFile() || !media::DecodedWaveCache::sealFile(wave).deleteFile()
                    || !wave.getSiblingFile(wave.getFileName() + ".decoding").deleteFile())
                { failed = true; std::cerr << "Owned packed cache retained after incomplete cleanup\n"; }
            }
        }
        properties.closeFiles();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr); look.reset(); chrome.reset();
        if (originalWorkingDirectory != juce::File() && !originalWorkingDirectory.setAsCurrentWorkingDirectory())
        { failed = true; std::cerr << "FAILED: could not restore test-process directory before fixture cleanup\n"; }
        if (fixture != juce::File() && !failed)
        {
            const bool owned = fixture.getParentDirectory() == parent && fixture.getFileName().startsWith("native-recovery-");
            if (!owned || !files::nativeFile(fixture).deleteRecursively())
            { failed = true; std::cerr << "Native fixture retained: " << fixture.getFullPathName() << '\n'; }
        }
        else if (failed && fixture != juce::File())
            std::cerr << "Failed native fixture retained for diagnosis: " << fixture.getFullPathName() << '\n';
        setApplicationReturnValue(failed ? 1 : 0);
        if (realBankInput != juce::File())
        {
            auto* report = new juce::DynamicObject;
            report->setProperty("schema", "singleeditor.native-real-bank-characterization");
            report->setProperty("passed", !failed);
            report->setProperty("bank_sha256", juce::SHA256(realBankInput.getChildFile("fixtures/bank/New_SoundBank.bnk")).toHexString());
            report->setProperty("reference_sha256", juce::SHA256(realBankReference).toHexString());
            report->setProperty("checks", juce::var(realBankChecks));
            report->setProperty("frames_compared", static_cast<juce::int64>(realBankFrames));
            report->setProperty("maximum_absolute_error", realBankMaxError);
            report->setProperty("event_association_qualified", false);
            report->setProperty("physical_playback_qualified", false);
            juce::Array<juce::var> extractions;
            for (const auto& file : realBankExtracted) extractions.add(file.getFullPathName());
            report->setProperty("owned_extractions", juce::var(extractions));
            const auto output = artifacts.getChildFile("real-bank.json");
            if (output.exists() || !output.replaceWithText(juce::JSON::toString(juce::var(report), true)))
            { failed = true; setApplicationReturnValue(1); }
            std::cout << "Native genuine bank characterization " << (failed ? "failed" : "passed") << '\n';
        }
        if (!failed && optionalLifecycleMode)
            std::cout << "Native optional lifecycle scenario passed (actual WAV CoreReady, EQ factory/View/Preview/Off/Remove, shared meter consumers and terminal resources; no physical playback or performance acceptance)\n";
        if (!failed && magLifecycleMode)
            std::cout << "Native Mag lifecycle scenario passed (real workspace transitions/generations/document close and bounded service lifetimes; no physical playback or performance acceptance)\n";
        if (!failed && trayScenario.isNotEmpty()) std::cout << "Native tray resource scenario passed: " << trayScenario << '\n';
        if (!failed && hrtfScenario.isNotEmpty()) std::cout << "Native HRTF resource scenario passed: " << hrtfScenario << '\n';
        if (!failed && bankScenario.isNotEmpty()) std::cout << "Native BNK scenario passed: " << bankScenario
            << ((bankScenario.startsWith("bank-open-") || bankScenario.startsWith("bank-fallback-")) ? " (actual Main parser admission, input leases, workspace and lifecycle assertions)\n"
                : " (actual Main media activation, complete PCM, owned decoder and lifecycle assertions)\n");
        if (!failed && realBankInput == juce::File() && !optionalLifecycleMode && !magLifecycleMode && bankScenario.isEmpty() && hrtfScenario.isEmpty() && trayScenario.isEmpty()) std::cout << (diagnosticsMode
            ? "NativeDiagnosticsTests passed (actual palette, passive providers, default privacy, reviewed local bundle, category opt-in, refresh, close and unchanged Undo/Redo)\n" : packedCloseMode
            ? "NativeRecoveryTests passed (actual WEM prefix cancellation on close, original readers/history/PCM returned, actual child exit before application shutdown)\n" : packedStartupMode
            ? "NativeRecoveryTests passed (startup WEM one bounded child, no fallback retry, usable empty GUI and subsequent ordinary audio)\n" : packedBudgetMode
            ? "NativeRecoveryTests passed (actual WEM output quota, explicit finite expansion, correct playlist owner, history and normal close)\n" : packedRecoveredMode
            ? "NativeRecoveryTests passed (actual Center restore, WEM late failure returns exact recovered owner/readers/full Undo/Redo/selection, ordinary close)\n" : packedEditedMode
            ? "NativeRecoveryTests passed (WEM late failure preserves edits made during loading and history, newer ordinary activation survives old result, actual child exit)\n" : packedBankMode
            ? "NativeRecoveryTests passed (actual embedded PCM bank preview, WEM late failure returns parsed bank selection/readers despite moved bank, source guard retained)\n" : packedMode
            ? "NativeRecoveryTests passed (actual WEM prefix, late failure/return, retry/fuse, cancellation, local diagnostics, completed edited history and ordinary close)\n" : videoStartupMode
            ? "NativeRecoveryTests passed (startup video invokes one bounded child, no fallback retry, usable empty GUI and subsequent ordinary audio)\n" : videoBudgetMode
            ? "NativeRecoveryTests passed (actual sparse output quota, explicit expanded-budget success, unchanged same-video edits, independent cache backing facts, real recovery protection and normal close)\n" : videoMode
            ? "NativeRecoveryTests passed (actual video child failure, explicit retry, session fuse, identity change, cancellation, complete current Undo/Redo preservation and explicit diagnostic export)\n" : earlierMode
            ? "NativeRecoveryTests passed (actual earlier-point selection, independent restore, exact earlier current/Undo/Redo without later edits, original workspace preservation and ordinary close)\n" : inspectionMode
            ? "NativeRecoveryTests passed (actual Safe Inspect, earlier points, full source hash, Missing/Locate/Relocated native chooser, redacted export, cancelled and confirmed Retry Once, full history, moved live source dirty guards, honest source-lease degradation and explicit exit)\n" : restoreSaveMode
            ? "NativeRecoveryTests passed (actual Restore Copy, DSP history, native Save As chooser/coordinator, independently read saved PCM, complete Undo/Redo after Save, unchanged historical source and ordinary close)\n" : restoreMode
            ? "NativeRecoveryTests passed (actual Restore Copy, failed scratch preparation, explicit new-attempt Retry, hidden Center, unselected playlist publication, full Undo/Redo, original source and current workspace preservation)\n" : retentionMode
            ? "NativeRecoveryTests passed (actual retention settings, Cancel, confirmed persistent policy, automatic cleanup after Center closes, fresh-view policy read/update, active owner and full current history, normal close)\n" : storageMode
            ? "NativeRecoveryTests passed (actual Storage view, physical totals and configured limits, Cancel scan, Records return, unchanged full current history, existing reviewed Discard/Retry and normal close)\n" : capacityMode
            ? "NativeRecoveryTests passed (real 10 minute 7.1 source, default full protection, exact durable history, nonmodal Recovery Center entry, unchanged full current history)\n" : cleanupScenario.isNotEmpty()
            ? (cleanupScenario.startsWith("cleanup-auto-")
                ? "NativeRecoveryTests passed (automatic PostCore persisted terminal cleanup, no Center or modal activation, unchanged source/current document, subsequent full Undo/Redo and ordinary close)\n"
                : "NativeRecoveryTests passed (persisted terminal marker/intent, selected Resume cleanup, real contention and Retry, closed View, unchanged source and complete current history)\n") : centerMode
            ? "NativeRecoveryTests passed (post-Core discovery, nonmodal Center, actual review, Cancel/second-confirmed Discard, background Retry, source/history preservation)\n" : invalidRoot
            ? "NativeRecoveryTests passed (actual missing recovery root, honest close prompt, Cancel/history, no relative writes and Exit Anyway)\n"
            : "NativeRecoveryTests passed (actual main window, recovery workers, Save command, diagnostics, Cancel/Retry, Simple status and Exit Anyway)\n");
    }
    void systemRequestedQuit() override { if (window) window->requestActualQuit(); else quit(); }

private:
    void seedRecoveryRecord()
    {
        SingleTrackDocument previous; check(previous.loadFile(source), "open previous-process recovery fixture");
        if (storageMode)
        {
            check(previous.moveEnvelopePoint(0, 0, 0.4f) && previous.moveEnvelopePoint(0, 0, 0.6f)
                && previous.undo() && previous.canRedo(), "prepare actual old full-history expiry fixture");
            check(static_cast<bool>(recovery::RecoveryStorageLayout(recoveryRoot).prepare()), "prepare private native expiry storage");
            const auto snapshot = previous.captureCommittedState(previous.getMediaIdentity().document);
            check(snapshot.has_value(), "capture actual old full-history expiry fixture");
            recovery::CommitPacket packet;
            check(static_cast<bool>(recovery::RecoveryCheckpointCodec::encode(*snapshot, {}, packet)), "encode native expiry fixture");
            packet.timestampMilliseconds = juce::Time::currentTimeMillis() - 2 * 86400000LL;
            firstOwner = juce::Uuid(); recovery::RecoveryStore store(journal, firstOwner); recovery::SafeHead head;
            std::shared_ptr<recovery::RecoveryOwnerLease> activity;
            check(static_cast<bool>(store.append({}, packet, head, nullptr, nullptr, &activity)),
                "publish single private old candidate with actual activity protocol then release it");
            sourceHash = juce::SHA256(source).toHexString();
            return;
        }
        DocumentSaveCoordinator saves(previous);
        recovery::RecoveryService::Config settings; settings.root = recoveryRoot;
        if (compactedRestoreMode) settings.sessions.session.producer.store.records = 2;
        recovery::RecoveryService service(previous, saves, settings);
        check(static_cast<bool>(service.attachPostCore(juce::Uuid())), "admit previous actual recovery fixture");
        if (restoreSaveMode)
        {
            auto processor = AudioSuiteReferenceProcessor::describe().create();
            const auto analyzed = processor->analyze(previous.getState(), 501, {}, 0.5f, 1.0);
            check(analyzed.succeeded() && processor->apply(previous, analyzed.plan, 501, {}).succeeded(),
                "seed actual committed DSP PCM distinct from the still-existing historical source");
        }
        check(previous.moveEnvelopePoint(0, 0, 0.35f), "commit previous fixture mutation");
        if (restoreMode)
            check(previous.moveEnvelopePoint(0, 0, 0.63f) && previous.undo(), "previous session contains real ordered Undo and Redo");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000.0;
        while (service.needsPoll() && juce::Time::getMillisecondCounterHiRes() < deadline)
        { service.poll(); juce::Thread::sleep(2); }
        check(protectedOwner(firstOwner), "previous owner has actual complete checkpoint");
        service.beginClose();
        while (!service.health().settled && juce::Time::getMillisecondCounterHiRes() < deadline)
        { if (service.needsPoll()) service.poll(); juce::Thread::sleep(2); }
        check(service.health().settled, "previous owner releases real producer and source leases");
        if (compactedRestoreMode)
        {
            const auto manifest = recovery::RecoveryStore(journal, firstOwner).inspectManifest();
            check(manifest.status && manifest.head.sequence == 4 && manifest.commits.size() == 1
                && manifest.checkpointCut.isNotEmpty() && manifest.checkpointFloor == manifest.head,
                "real producer compacted four DSP/envelope/Undo events into one complete native recovery checkpoint");
            std::cout << "Native compacted recovery fixture: head=" << manifest.head.sequence
                << " retained_records=" << manifest.commits.size() << " cut=" << manifest.checkpointCut << '\n';
        }
        if (cleanupScenario.isNotEmpty())
        {
            recovery::RecoveryStore store(journal, firstOwner);
            const auto manifest = store.inspectManifest();
            recovery::OwnerRetirement marker;
            check(manifest.status && recovery::RecoveryOwnerLifecycle::discard(store, manifest.head, marker),
                "previous fixture explicitly publishes its actual selected-owner Discard marker");
            if (cleanupScenario.endsWith("intent"))
            {
                recovery::TerminalCollectionReport report;
                for (int pass = 0; pass < 8 && report.filesRemoved == 0; ++pass)
                    check(static_cast<bool>(store.collectTerminal(manifest.head, report, { 1, 32 * 1024 * 1024 })),
                        "persist a real interrupted bounded sweep before creating the new MainWindow");
                check(report.filesRemoved > 0 && report.more && !report.completed,
                    "native restart begins with partially collected data and its durable inventory");
            }
            // Preserve the explicit Resume coverage now that normal startup
            // automatically collects terminal data. The real failed job waits
            // for the existing Retry/Resume interaction after this lock releases.
            if (!cleanupScenario.startsWith("cleanup-auto-")) holdQuota();
        }
        if (inspectionMode)
        {
            const auto manifest = recovery::RecoveryStore(journal, firstOwner).inspectManifest();
            check(manifest.status && manifest.commits.size() >= 3, "inspection fixture has actual ordered safe points");
            for (int i = 0; i < 2; ++i)
            {
                std::unique_ptr<recovery::RecoveryAttemptWriter> attempt;
                check(static_cast<bool>(recovery::RecoveryAttemptStore(recovery::RecoveryStorageLayout(recoveryRoot).attemptDirectory(), firstOwner)
                    .begin(juce::Uuid(), manifest.head, manifest.head, getApplicationVersion(), attempt)), "seed real unclosed attempt marker");
                attempt.reset();
            }
        }
        sourceHash = juce::SHA256(source).toHexString();
    }
    static juce::DocumentWindow* centerWindow()
    {
        for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
            if (auto* window = dynamic_cast<juce::DocumentWindow*>(juce::Desktop::getInstance().getComponent(i));
                window && window->getName() == "Recovery Center") return window;
        return nullptr;
    }
    static juce::String centerText(const juce::String& name = "Recovery record details")
    {
        const auto find = [&name](auto&& self, juce::Component& component) -> juce::String
        {
            if (auto* editor = dynamic_cast<juce::TextEditor*>(&component); editor && editor->getName() == name)
                return editor->getText();
            for (auto* child : component.getChildren()) if (auto text = self(self, *child); text.isNotEmpty()) return text;
            return {};
        };
        if (auto* window = centerWindow()) return find(find, *window);
        return {};
    }
    static void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
    void fail(const juce::String& reason)
    {
        failed = true; std::cerr << "FAILED at native stage " << stage << ": " << reason << '\n';
        if (window && window->getMainComponent())
            if (const auto* service = window->getMainComponent()->recoveryServiceForTesting())
                try
                {
                    const auto health = service->health();
                    std::cerr << "Recovery state: poll=" << health.needsPoll << " pending=" << health.protection.pendingPackets
                        << " protection=" << health.protection.failure.code << " disposition=" << health.dispositionFailure.code << '\n';
                    size_t recorded = 0;
                    for (const auto& session : health.protection.sessions)
                    {
                        if (++recorded > 16) { std::cerr << "Remaining sessions omitted\n"; break; }
                        const auto& p = session.protection;
                        std::cerr << "Session: active=" << session.active << " closed=" << p.closed << " gap=" << p.checkpointRequired
                            << " currentProtected=" << p.currentProtected << " failure=" << p.failure.code << " retired=" << p.retired.size() << '\n';
                        const auto epoch = [](const auto& e)
                        { std::cerr << "  Epoch: owner=" << e.owner.toString() << " pending=" << e.writer.pendingPackets
                            << " running=" << e.writer.workerRunning << " gap=" << e.checkpointRequired << " failure=" << e.failure.code
                            << " writer=" << e.writer.failure.code << " sequence=" << e.writer.safeHead.sequence << '\n'; };
                        epoch(p.current);
                        for (size_t i = 0; i < std::min<size_t>(p.retired.size(), 16); ++i) epoch(p.retired[i]);
                    }
                }
                catch (...) { std::cerr << "Recovery state observation unavailable\n"; }
        if (packedMode) std::cerr << "Actual packed decoder PIDs: " << decoderPids().joinIntoString(",") << '\n';
        if (window && artifacts.isDirectory())
            try { render(*window, "failed-client-render.png"); } catch (...) { std::cerr << "Failure render unavailable\n"; }
        if (auto* center = centerWindow())
        { std::cerr << "Center details: " << centerText() << "\nTasks: " << centerText("Recovery restore task details") << '\n'; try { render(*center, "failed-center-render.png"); } catch (...) {} }
        if (auto* modal = juce::Component::getCurrentlyModalComponent())
            std::cerr << "Modal: " << modal->getName() << " " << modal->getDescription() << '\n';
        setApplicationReturnValue(1); stopTimer(); quit();
    }
    void makeSource()
    {
        juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
        const auto channels = hrtfScenario.isEmpty() ? 2 : 4;
        auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(channels).withBitsPerSample(24));
        check(writer != nullptr, "create native source writer");
        juce::AudioBuffer<float> pcm(channels, 48000);
        for (int c = 0; c < channels; ++c) for (int i = 0; i < pcm.getNumSamples(); ++i)
            pcm.setSample(c, i, ((i * 13 + c) % 139 - 69) / 150.0f);
        check(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write owned native source");
    }
    void next(int value) { stage = value; stageStarted = juce::Time::getMillisecondCounterHiRes(); std::cout << "native stage " << stage << std::endl; }
    static juce::Button* button(juce::Component& root, const juce::String& label)
    {
        if (auto* item = dynamic_cast<juce::Button*>(&root); item && item->isShowing()
            && (item->getButtonText() == label || item->getName() == label || item->getComponentID() == label)) return item;
        for (auto* child : root.getChildren()) if (auto* found = button(*child, label)) return found;
        return nullptr;
    }
    static bool clickModal(const juce::String& label)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        if (!modal) return false;
        auto* action = button(*modal, label);
        if (!action || !action->isEnabled()) return false;
        action->triggerClick(); return true;
    }
    static juce::ComboBox* combo(juce::Component& root, const juce::String& name)
    {
        if (auto* item = dynamic_cast<juce::ComboBox*>(&root); item && item->getName() == name) return item;
        for (auto* child : root.getChildren()) if (auto* found = combo(*child, name)) return found;
        return nullptr;
    }
    bool chooseOwnedFile(const juce::File& file, const juce::File& second = {})
    {
        check(file.isAChildOf(fixture) || file.isAChildOf(artifacts), "native chooser only receives owned fixture or artifact paths");
        check(second == juce::File() || second.isAChildOf(fixture) || second.isAChildOf(artifacts), "second selected file is test-owned");
#if JUCE_WINDOWS
        struct Dialog { HWND window = nullptr, edit = nullptr; } dialog;
        EnumWindows([](HWND window, LPARAM data) -> BOOL
        {
            DWORD process = 0; GetWindowThreadProcessId(window, &process);
            wchar_t type[64]{}; GetClassNameW(window, type, 64);
            if (process != GetCurrentProcessId() || !IsWindowVisible(window) || juce::String(type) != "#32770") return TRUE;
            auto& found = *reinterpret_cast<Dialog*>(data); found.window = window;
            EnumChildWindows(window, [](HWND child, LPARAM state) -> BOOL
            {
                wchar_t childType[64]{}, parentType[64]{}; GetClassNameW(child, childType, 64);
                GetClassNameW(GetParent(child), parentType, 64);
                if (juce::String(childType) == "Edit" && IsWindowVisible(child) && juce::String(parentType).startsWith("ComboBox"))
                { reinterpret_cast<Dialog*>(state)->edit = child; return FALSE; }
                return TRUE;
            }, data);
            return found.edit ? FALSE : TRUE;
        }, reinterpret_cast<LPARAM>(&dialog));
        if (!dialog.window || !dialog.edit) return false;
        DWORD_PTR ignored = 0;
        const auto selection = second == juce::File() ? file.getFullPathName()
            : "\"" + file.getFullPathName() + "\" \"" + second.getFullPathName() + "\"";
        check(SendMessageTimeoutW(dialog.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(selection.toWideCharPointer()),
            SMTO_ABORTIFHUNG, 1000, &ignored) != 0, "set only this process's native file-name control");
        PostMessageW(dialog.window, WM_COMMAND, IDOK, 0); return true;
#else
        return false;
#endif
    }
    bool statusVisible() const
    {
        auto* client = window->getMainComponent();
        return client && button(*client, "Recovery status and actions") != nullptr;
    }
    bool chooseOwnedInitialProjectDirectory()
    {
#if JUCE_WINDOWS
        HWND dialog = nullptr;
        EnumWindows([](HWND candidate, LPARAM data) -> BOOL
        {
            DWORD process = 0; GetWindowThreadProcessId(candidate, &process);
            wchar_t type[64]{}, title[256]{};
            GetClassNameW(candidate, type, 64); GetWindowTextW(candidate, title, 256);
            if (process == GetCurrentProcessId() && IsWindowVisible(candidate) && juce::String(type) == "#32770"
                && juce::String(title) == "Attach Wwise Project Root")
            { *reinterpret_cast<HWND*>(data) = candidate; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&dialog));
        if (!dialog || !IsWindowEnabled(GetDlgItem(dialog, IDOK))) return false;
        // Main initialises this directory picker from the bank's actual parent.
        // Accept its real Select Folder action, then independently assert the
        // adopted project root is exactly our private fixture in stage 632.
        PostMessageW(dialog, WM_COMMAND, IDOK, 0); return true;
#else
        return false;
#endif
    }
    static bool clearOfVisibleWaveform(juce::Component& root, juce::Rectangle<int> actionBounds)
    {
        if (dynamic_cast<WaveformViewComponent*>(&root) && root.isShowing()
            && root.getScreenBounds().intersects(actionBounds)) return false;
        for (auto* child : root.getChildren()) if (!clearOfVisibleWaveform(*child, actionBounds)) return false;
        return true;
    }
    void checkStatusLayout() const
    {
        auto* client = window->getMainComponent();
        auto* status = button(*client, "Recovery status and actions");
        check(status && clearOfVisibleWaveform(*client, status->getScreenBounds()),
            "recovery action does not cover the visible waveform or its navigation controls");
        check(client->getComponentAt(status->getBounds().getCentre()) == status,
            "recovery action remains reachable by pointer hit testing");
    }
    void clickStatus()
    {
        auto* client = window->getMainComponent();
        auto* status = client ? button(*client, "Recovery status and actions") : nullptr;
        check(status != nullptr, "pending/degraded status is visible and operable"); status->triggerClick();
    }
    void activatePlaylistRow(int index)
    {
        const auto find = [](auto&& self, juce::Component& component) -> PlaylistSidebarComponent*
        {
            if (auto* sidebar = dynamic_cast<PlaylistSidebarComponent*>(&component)) return sidebar;
            for (auto* child : component.getChildren()) if (auto* found = self(self, *child)) return found;
            return nullptr;
        };
        auto* sidebar = find(find, *window->getMainComponent());
        check(sidebar && sidebar->onEntryActivated, "real playlist activation callback exists");
        sidebar->onEntryActivated(index); // Includes Main's actual Save/Discard/Cancel route.
    }
    template<class T> static T* findComponent(juce::Component& root)
    {
        if (auto* found = dynamic_cast<T*>(&root)) return found;
        for (auto* child : root.getChildren()) if (auto* found = findComponent<T>(*child)) return found;
        return nullptr;
    }
    void retainPackedSelection()
    {
        auto* view = findComponent<WaveformViewComponent>(*window);
        check(view != nullptr, "actual native waveform selection view exists");
        view->restoreSelection(juce::Range<double>(0.1, 0.4));
    }
    void checkPackedSelection()
    {
        auto* view = findComponent<WaveformViewComponent>(*window);
        check(view && view->getSelectionRange() == std::optional<juce::Range<double>>(juce::Range<double>(0.1, 0.4)),
            "native return preserves actual waveform selection range");
    }
    bool packedHistoryProtected() const
    {
        for (const auto& owner : owners())
        {
            recovery::RecoveryStore store(journal, owner);
            const auto manifest = store.inspectManifest();
            recovery::PreparedRecoveryCheckpoint prepared;
            if (!manifest.status || !recovery::RecoveryCheckpointCodec::prepareThrough(manifest, manifest.head, prepared)) continue;
            const auto& states = prepared.getStates();
            if (states.empty() || states.front()->info.file != videoSource
                || prepared.getUndoCount() != 1 || prepared.getRedoCount() != 1
                || std::abs(states.front()->envelopePoints.front().gain - 0.7f) > 1.0e-6f) continue;
            return static_cast<bool>(store.inspect().status);
        }
        return false;
    }
    void render(juce::Component& component, const char* name)
    {
        // Real native JUCE client rendering; not a physical screen capture or a
        // benchmark. The operating-system frame/glass is a separate acceptance.
        const auto picture = component.createComponentSnapshot(component.getLocalBounds());
        auto output = artifacts.getChildFile(name).createOutputStream();
        check(output != nullptr && picture.isValid(), "create native client-render artifact");
        juce::PNGImageFormat png; check(png.writeImageToStream(picture, *output), "write native client-render artifact");
    }
    void checkBankMetadataLayout(bool hasRtpc, const char* scrolledImage)
    {
        auto* metadata = findComponent<MetadataSidebarComponent>(*window);
        auto* viewport = metadata ? dynamic_cast<juce::Viewport*>(diagnosticChild(*metadata, "bank.metadata.viewport")) : nullptr;
        check(viewport && viewport->isShowing() && viewport->getViewedComponent(),
            "actual BNK metadata has a visible scrollable content surface");
        auto* content = viewport->getViewedComponent();
        check(content->getHeight() > viewport->getViewHeight() && viewport->getVerticalScrollBar().isVisible(),
            "default-size metadata exposes overflow through its vertical scrollbar");
        int previousBottom = 0;
        juce::Component* rtpc = nullptr;
        for (auto* field : content->getChildren())
        {
            check(field->getY() >= previousBottom && field->getHeight() > 0
                && field->getRight() <= content->getWidth(), "all actual bank metadata rows have separate ordered bounds");
            previousBottom = field->getBottom();
            if (field->getName() == "RTPC") rtpc = field;
        }
        check((rtpc != nullptr) == hasRtpc, "actual metadata rows retain exact-target RTPC visibility");
        auto* target = rtpc ? rtpc : content->getChildren().getLast();
        viewport->getVerticalScrollBar().setCurrentRangeStart(target->getY(), juce::sendNotificationSync);
        const auto visible = viewport->getViewArea();
        check(visible.intersects(target->getBounds()) && visible.getY() > 0,
            "the real scrollbar reveals metadata below the initial viewport");
        render(*metadata, scrolledImage);
        viewport->getVerticalScrollBar().setCurrentRangeStart(0, juce::sendNotificationSync);
    }
    std::vector<juce::Uuid> owners() const
    {
        std::vector<juce::Uuid> result;
        for (const auto& directory : files::nativeFile(journal).findChildFiles(juce::File::findDirectories, false))
            if (directory.getFileName().length() == 32) result.emplace_back(directory.getFileName());
        return result;
    }
    bool protectedOwner(juce::Uuid& output) const
    {
        for (const auto& owner : owners())
        {
            const auto view = recovery::RecoveryStore(journal, owner).inspectManifest();
            if (view.status && view.head.sequence > 0) { output = owner; return true; }
        }
        return false;
    }
    void saveThroughNativeCommand()
    {
        check(document->getFileInfo().file.isAChildOf(fixture), "native Save can only overwrite the isolated source");
        auto* client = window->getMainComponent();
        // Ctrl+S belongs to the workspace key handler. The top-level caption
        // fallback deliberately handles only the palette and transport keys.
        check(client && client->keyPressed(juce::KeyPress('s', juce::ModifierKeys::ctrlModifier, 0)),
            "actual native Ctrl+S command route accepts the saved-document action");
    }
    std::vector<float> restoredDspHistoryPcm()
    {
        const auto history = document->captureCommittedState(document->getMediaIdentity().document);
        check(history && history->undo.size() == 2 && history->redo.size() == 1,
            "native recovered DSP retains both Undo states and the future envelope state");
        check(document->undo() && document->undo() && !document->canUndo(), "reach original PCM through the full real Undo stack");
        std::vector<float> result;
        for (int state = 0; state < 4; ++state)
        {
            for (const int64_t start : { int64_t(1024), int64_t(24000), int64_t(47000) })
            {
                juce::AudioBuffer<float> pcm(2, 64);
                check(engine->renderDocumentBlockForDiagnostics(start, pcm), "render every recovered historical state through the native engine");
                for (int channel = 0; channel < 2; ++channel) for (int sample = 0; sample < 64; ++sample)
                    result.push_back(pcm.getSample(channel, sample));
            }
            if (state < 3) check(document->redo(), "traverse every real recovered Redo state");
        }
        check(!document->canRedo() && document->undo(), "return to the selected safe point after full history traversal");
        return result;
    }
    void verifyNativeRecoveredSave()
    {
        const auto saved = artifacts.getChildFile("recovered-dsp-saved.wav");
        auto reader = SingleTrackDocument::createReaderForFile(saved);
        auto original = SingleTrackDocument::createReaderForFile(source);
        check(reader && original && reader->lengthInSamples == 48000 && reader->numChannels == 2
            && reader->sampleRate == 48000, "native Save As writes the complete actual audio dimensions");
        float maximumError = 0.0f;
        for (const int64_t start : { int64_t(1024), int64_t(24000), int64_t(47000) })
        {
            juce::AudioBuffer<float> input(2, 64), output(2, 64);
            check(original->read(&input, 0, 64, start, true, true) && reader->read(&output, 0, 64, start, true, true),
                "independently read historical source and physically saved WAV");
            for (int channel = 0; channel < 2; ++channel) for (int sample = 0; sample < 64; ++sample)
            {
                const auto envelope = SingleTrackDocument::evaluateEnvelope(restoredSaveState->envelopePoints,
                    static_cast<double>(start + sample) / 48000.0);
                const auto error = std::abs(output.getSample(channel, sample) - input.getSample(channel, sample) * 0.5f * envelope);
                maximumError = std::max(maximumError, error);
                check(error < 1.1f / 32768.0f, "native recovered Save contains DSP and envelope, never historical source PCM");
            }
        }
        const auto after = restoredDspHistoryPcm();
        check(after.size() == restoredSaveHistoryPcm.size(), "native Save preserves the whole history sample oracle");
        for (size_t i = 0; i < after.size(); ++i)
            check(std::abs(after[i] - restoredSaveHistoryPcm[i]) < 1.0e-6f, "every Undo/Redo state plays identically after native Save As");
        check(juce::SHA256(source).toHexString() == sourceHash, "native recovered Save never changes the historical source");
        auto* receipt = new juce::DynamicObject();
        receipt->setProperty("savedSha256", juce::SHA256(saved).toHexString());
        receipt->setProperty("sourceSha256", sourceHash);
        receipt->setProperty("frames", juce::var(int64_t(48000)));
        receipt->setProperty("channels", 2);
        receipt->setProperty("savedPcmSamplesChecked", 384);
        receipt->setProperty("historyPcmSamplesChecked", static_cast<int>(after.size()));
        receipt->setProperty("maximumSavedPcmError", maximumError);
        receipt->setProperty("nativeSaveAs", true);
        check(artifacts.getChildFile("recovered-save-pcm.json").replaceWithText(juce::JSON::toString(juce::var(receipt))),
            "retain actual native Save content and history receipt");
    }
    void holdQuota()
    {
        quotaBlock = std::make_unique<juce::FileOutputStream>(files::nativeFile(recoveryRoot.getChildFile("quota.lock")));
        check(quotaBlock->openedOk(), "hold the real private service transaction file");
    }
    void createNativeBank()
    {
        juce::MemoryBlock pcm; check(source.loadFileAsData(pcm), "read owned PCM bank payload");
        juce::MemoryOutputStream out;
        out.write("BKHD", 4); out.writeInt(8); out.writeInt(128); out.writeInt(7);
        out.write("DIDX", 4); out.writeInt(4 * 12);
        for (int i = 0; i < 4; ++i)
        { out.writeInt(99 + i); out.writeInt(static_cast<int>(i * pcm.getSize())); out.writeInt(static_cast<int>(pcm.getSize())); }
        out.write("DATA", 4); out.writeInt(static_cast<int>(4 * pcm.getSize()));
        for (int i = 0; i < 4; ++i) out.write(pcm.getData(), pcm.getSize());
        packedBankFile = fixture.getChildFile(bankScenario.startsWith("bank-fallback-") ? "disguised-bank.wav" : "native-preview.bnk");
        check(packedBankFile.replaceWithData(out.getData(), out.getDataSize()), "create real bank with embedded and streamed native media");
        if (bankScenario == "bank-media-locators")
        {
            check(fixture.getChildFile("locator.wproj").replaceWithText("<WwiseDocument/>"),"create owned locator project");
            const auto original=fixture.getChildFile("Originals/exact.wav");
            check(original.getParentDirectory().createDirectory().wasOk(),"create owned original folder");
            {
                juce::WavAudioFormat format;
                std::unique_ptr<juce::OutputStream> stream(original.createOutputStream());
                const auto options=juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(1).withBitsPerSample(16);
                std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream,options));
                juce::AudioBuffer<float> originalPcm(1,144000);
                for (int i=0;i<144000;++i) originalPcm.setSample(0,i,0.5f);
                check(writer && writer->writeFromAudioSampleBuffer(originalPcm,0,144000),"write actual distinct three-second original PCM");
            }
            check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText(
                "<SoundBanksInfo><SoundBanks><SoundBank><Path>native-preview.bnk</Path><IncludedEvents>"
                "<Event Id=\"203\" Name=\"Original and generated\"><ReferencedStreamedFiles><File Id=\"100\"/>"
                "</ReferencedStreamedFiles></Event></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),"create native locator XML");
            check(packedBankFile.withFileExtension(".txt").replaceWithText(
                "In Memory Audio\tID\tName\tAudio source file\tWwise Object Path\n100\t\tOriginals/exact.wav\t\n"
                "Streamed Audio\tID\tName\tAudio source file\tGenerated audio file\tWwise Object Path\n"
                "100\t\tOriginals/exact.wav\t100.wem\t\n"),"create native TXT with empty name/owner and both locator roles");
            finishBankInput(100);
        }
        if (bankScenario == "bank-state-scope" || bankScenario == "bank-rtpc-metadata")
        {
            check(fixture.getChildFile("scope.wproj").replaceWithText("<WwiseDocument/>"), "create actual discoverable owned project");
            const auto events = fixture.getChildFile("Events"), actors = fixture.getChildFile("Actor-Mixer Hierarchy");
            check(events.createDirectory().wasOk() && actors.createDirectory().wasOk(), "create actual native authoring work units");
            const auto target = [](const char* id)
            { return juce::String("<Action><ReferenceList><Reference Name=\"Target\"><ObjectRef ID=\"") + id + "\"/></Reference></ReferenceList></Action>"; };
            check(events.getChildFile("events.wwu").replaceWithText(
                "<WwiseDocument><Event ID=\"event-single\" Name=\"Native single target\"><ChildrenList>" + target("target-a")
                + "</ChildrenList></Event><Event ID=\"event-multi\" Name=\"Native multiple targets\"><ChildrenList>"
                + target("target-a") + target("target-b") + "</ChildrenList></Event>"
                + (bankScenario == "bank-rtpc-metadata" ? "<Event ID=\"event-other\" Name=\"Native other target\"><ChildrenList>"
                    + target("target-b") + "</ChildrenList></Event>" : juce::String()) + "</WwiseDocument>"), "write native single and multi-target authoring events");
            const auto states = [this](const char* id, const char* name, const char* db)
            {
                const juce::String parameterBinding = bankScenario == "bank-rtpc-metadata" && juce::String(id)=="target-a"
                    ? "<PropertyList><Property Name=\"Volume\"><RTPCList><RTPC><ReferenceList>"
                      "<Reference Name=\"ControlInput\"><ObjectRef ID=\"rtpc-right\" Name=\"Speed\"/></Reference>"
                      "</ReferenceList><Curve><PointList><Point><XPos>0</XPos><YPos>-30</YPos></Point>"
                      "</PointList></Curve></RTPC></RTPCList></Property></PropertyList>" : juce::String();
                return juce::String("<Sound ID=\"") + id + "\" Name=\"" + name + "\">" + parameterBinding + "<StateInfo><StateGroupList>"
                    "<StateGroupInfo><StateGroupRef ID=\"group-shadow\" Name=\"Weather\"/></StateGroupInfo>"
                    "<StateGroupInfo><StateGroupRef ID=\"group-weather\" Name=\"Weather\"/></StateGroupInfo></StateGroupList><CustomStateList>"
                    "<CustomState><StateRef ID=\"state-rain\" Name=\"Rain\"/><CustomState><PropertyList><Property Name=\"Volume\" Value=\""
                    + db + "\"/></PropertyList></CustomState></CustomState><CustomState><StateRef ID=\"state-none\" Name=\"None\"/>"
                    "<CustomState><PropertyList><Property Name=\"Volume\" Value=\"0\"/></PropertyList></CustomState></CustomState>"
                    "<CustomState><StateRef ID=\"shadow-rain\" Name=\"Rain\"/><CustomState><PropertyList>"
                    "<Property Name=\"Volume\" Value=\"3\"/></PropertyList></CustomState></CustomState>"
                    "</CustomStateList></StateInfo></Sound>";
            };
            check(actors.getChildFile("actors.wwu").replaceWithText("<WwiseDocument>" + states("target-a", "Native target", "-6")
                + states("target-b", bankScenario=="bank-rtpc-metadata" ? "Native target" : "target-a", "3")
                + "</WwiseDocument>"), "native object name cannot overwrite the first object's identity");
            const auto eventMedia = [](int id, const char* guid, const char* name)
            {
                return "<Event Id=\"" + juce::String(id) + "\" GUID=\"" + guid + "\" Name=\"" + name
                    + "\"><ReferencedStreamedFiles><File Id=\"100\"><ShortName>100.wem</ShortName><Path>100.wem</Path></File>"
                    "</ReferencedStreamedFiles></Event>";
            };
            check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText(
                juce::String("<SoundBanksInfo><SoundBanks><SoundBank><Path>native-preview.bnk</Path>")
                + (bankScenario=="bank-rtpc-metadata" ? "<GameParameters>"
                    "<GameParameter Id=\"301\" GUID=\"rtpc-left\" Name=\"Speed\" Min=\"0\" Max=\"10\"/>"
                    "<GameParameter Id=\"302\" GUID=\"rtpc-right\" Name=\"Speed\" Min=\"0\" Max=\"20\"/>"
                    "</GameParameters>" : juce::String()) + "<StateGroups>"
                "<StateGroup GUID=\"group-shadow\" Name=\"Weather\"><States>"
                "<State Id=\"1\" GUID=\"shadow-none\" Name=\"None\"/><State Id=\"2\" GUID=\"shadow-rain\" Name=\"Rain\"/>"
                "</States></StateGroup><StateGroup GUID=\"group-weather\" Name=\"Weather\"><States>"
                "<State Id=\"1\" GUID=\"state-none\" Name=\"None\"/><State Id=\"2\" GUID=\"state-rain\" Name=\"Rain\"/>"
                "</States></StateGroup></StateGroups><IncludedEvents>" + eventMedia(203, "event-single", "Native single target")
                + eventMedia(204, "event-multi", "Native multiple targets")
                + (bankScenario=="bank-rtpc-metadata" ? eventMedia(205,"event-other","Native other target") : juce::String())
                + "</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"), "write actual native authoring identity sidecar");
            finishBankInput(100);
        }
        if (bankScenario == "bank-event" || bankScenario == "bank-switch")
        {
            const bool switches = bankScenario == "bank-switch";
            juce::String xml = "<SoundBanksInfo><SoundBanks><SoundBank><Path>native-preview.bnk</Path>";
            if (switches)
                xml += "<SwitchGroups><SwitchGroup Name=\"Native Switch\"><Switches>"
                    "<Switch Id=\"11\" GUID=\"native-a\" Name=\"Native A\"/><Switch Id=\"12\" GUID=\"native-b\" Name=\"Native B\"/>"
                    "</Switches></SwitchGroup></SwitchGroups>";
            xml += "<IncludedEvents><Event Id=\"" + juce::String(switches ? 202 : 201) + "\" Name=\"Native event\"><ReferencedStreamedFiles>"
                "<File Id=\"100\"><ShortName>100.wem</ShortName><Path>100.wem</Path></File>"
                "<File Id=\"101\"><ShortName>101.wem</ShortName><Path>101.wem</Path></File></ReferencedStreamedFiles>";
            if (switches)
                xml += "<DefaultSwitchValue>native-a</DefaultSwitchValue><SwitchContainers>"
                    "<SwitchContainer SwitchValue=\"native-a\"><Media><File Id=\"100\"/></Media></SwitchContainer>"
                    "<SwitchContainer SwitchValue=\"native-b\"><Media><File Id=\"101\"/></Media></SwitchContainer></SwitchContainers>";
            xml += "</Event></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>";
            check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText(xml), "create actual bounded sidecar input for native event controls");
            if (!switches) { finishBankInput(100); finishBankInput(101); }
        }
    }
    void activateBankEvent(uint32_t id)
    {
        auto* events = findComponent<BnkEventListViewComponent>(*window);
        auto* list = events ? findComponent<juce::ListBox>(*events) : nullptr;
        check(list && list->getListBoxModel(), "actual native bank event ListBox model exists");
        const auto parsed = bank->getState();
        const auto& descriptors = parsed->events;
        const auto found = std::find_if(descriptors.begin(), descriptors.end(), [id](const auto& event) { return event.id == id; });
        if (found == descriptors.end())
            std::cerr << "Bank sidecar diagnostic: " << parsed->info.diagnostics.joinIntoString(" | ") << '\n';
        check(found != descriptors.end(), "actual sidecar parser populated requested bank event");
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent click(juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, list, list, now, {}, now, 1, false);
        list->getListBoxModel()->listBoxItemClicked(static_cast<int>(std::distance(descriptors.begin(), found)), click);
        check(bank->getSelectedEventId() == id, "actual native list model selects and activates the sidecar event");
    }
    static juce::String componentText(juce::Component& component)
    {
        juce::String result;
        if (auto* label = dynamic_cast<juce::Label*>(&component)) result = label->getText();
        for (auto* child : component.getChildren()) result += "\n" + componentText(*child);
        return result;
    }
    bool bankOpenInputsReleased() const
    {
#if JUCE_WINDOWS
        std::vector<juce::File> inputs { packedBankFile, queuedOpenBank, fixture.getChildFile("SoundbanksInfo.xml") };
        const auto actors = fixture.getChildFile("Actor-Mixer Hierarchy");
        if (bankScenario == "bank-open-derived")
            for (int i = 0; i < 4; ++i) inputs.push_back(actors.getChildFile(juce::String(i) + ".wwu"));
        for (const auto& input : inputs)
        {
            if (input == juce::File() || !input.existsAsFile()) continue;
            check(input.getParentDirectory() == fixture || (bankScenario == "bank-open-derived" && input.getParentDirectory() == actors),
                "bank open lease probe is restricted to the owned fixture");
            const auto handle = CreateFileW(input.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            CloseHandle(handle);
        }
#endif
        return true;
    }
    void checkBankOpenInputsReleased() const
    {
        check(bankOpenInputsReleased(), "settled native bank open releases its actual bank and metadata deny-write leases");
    }
    void startBankOpenScenario(MainComponent& client)
    {
        createNativeBank();
        check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(),
            "bank open begins with actual clean audio and retained Redo");
        if (bankScenario.startsWith("bank-fallback-"))
        {
            check(document->redo() && document->moveEnvelopePoint(0, 0, 0.72f) && document->moveEnvelopePoint(0, 0, 0.31f)
                && document->undo() && document->undo() && document->undo(), "create three actual Redo states before disguised-bank fallback");
            for (int i = 0; i < 3; ++i)
            {
                check(document->redo(), "exercise original Redo before fallback");
                juce::AudioBuffer<float> pcm(2, 128);
                check(engine->renderDocumentBlockForDiagnostics(0, pcm), "capture real PCM for each original Redo state");
                fallbackRedoPcm.push_back(std::move(pcm));
            }
            check(document->undo() && document->undo() && document->undo(), "return original history to its actual saved state");
            fallbackHistory = document->captureCommittedState(document->getMediaIdentity().document);
            check(fallbackHistory && fallbackHistory->redo.size() == 3, "capture the entire actual fallback history before replacement");
        }
        videoOriginalState = document->getState();
        check(engine->renderDocumentBlockForDiagnostics(0, videoOriginalPcm), "capture actual pre-open engine PCM");
        retainPackedSelection(); bankWorkspace = document->captureWorkspaceSnapshot();
        check(bankWorkspace != nullptr, "capture complete workspace before native bank parsing");
        if (bankScenario == "bank-open-core" || bankScenario == "bank-fallback-core" || bankScenario == "bank-fallback-close"
            || bankScenario == "bank-fallback-superseded")
        {
            if (bankScenario.startsWith("bank-fallback-"))
            {
                juce::MemoryOutputStream broken;
                broken.write("BKHD", 4); broken.writeInt(8); broken.writeInt(128); broken.writeInt(7);
                broken.write("HIRC", 4); broken.writeInt(512);
                check(packedBankFile.replaceWithData(broken.getData(), broken.getDataSize()), "create actual bank signature with truncated core under ordinary WAV extension");
            }
            else check(packedBankFile.replaceWithText("invalid-core-bytes"), "create actual malformed core input");
            client.openInitialFile(packedBankFile);
            if (bankScenario == "bank-fallback-close") { expectedQuit = true; client.requestCloseApplication(); next(450); }
            else if (bankScenario == "bank-fallback-superseded")
            {
                fallbackNewAudio = fixture.getChildFile("newer-ordinary.wav");
                check(source.copyFileTo(fallbackNewAudio), "copy actual owned PCM into the newer ordinary input");
                client.openInitialFile(fallbackNewAudio); next(665);
            }
            else next(600);
            return;
        }
        if (bankScenario == "bank-open-derived")
        {
            check(fixture.getChildFile("derived.wproj").replaceWithText("<WwiseDocument/>"), "create discoverable owned derived-path project");
            const auto actors = fixture.getChildFile("Actor-Mixer Hierarchy");
            check(actors.createDirectory().wasOk(), "create actual owned authoring hierarchy");
            for (int i = 0; i < 4; ++i)
            {
                juce::String xml = "<WwiseDocument><WorkUnit Name=\"" + juce::String::repeatedString("x", 8192) + "\"><ChildrenList>";
                for (int child = 0; child < 256; ++child)
                {
                    const auto id = juce::String(i * 256 + child);
                    xml += "<Sound ID=\"object-" + id + "\" Name=\"Sound-" + id + "\"/>";
                }
                xml += "</ChildrenList></WorkUnit></WwiseDocument>";
                check(actors.getChildFile(juce::String(i) + ".wwu").replaceWithText(xml), "write bounded native derived-path input");
            }
            check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText(
                "<SoundBanksInfo><SoundBanks><SoundBank><Path>native-preview.bnk</Path><IncludedEvents>"
                "<Event Id=\"909\" Name=\"must not publish\"/></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),
                "create valid event catalog whose optional transaction must remain unpublished on path budget failure");
            client.openInitialFile(packedBankFile); next(620); return;
        }
        if (bankScenario == "bank-open-sidecar")
        {
            check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText(
                "<SoundBanksInfo><SoundBanks><SoundBank><Path>native-preview.bnk</Path>"
                "<GameParameters><GameParameter Id=\"3\" Name=\"Speed\" Min=\"0\" Max=\"12suffix\"/></GameParameters><IncludedEvents>"
                "<Event Id=\"909\" Name=\"must not publish\"/>"
                "</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),
                "create actual numeric sidecar failure without publishing a partial parameter or event");
            client.openInitialFile(packedBankFile); next(620); return;
        }
        // A real, bounded table keeps the parser occupied while the message
        // thread exercises cancellation. No artificial worker gate or mock.
        juce::MemoryOutputStream table;
        constexpr int objects = 60000, payloadSize = 256;
        table.write("HIRC", 4); table.writeInt(4 + objects * (1 + 4 + 4 + payloadSize)); table.writeInt(objects);
        const std::array<char, payloadSize> payload {};
        for (int i = 0; i < objects; ++i)
        {
            table.writeByte(2); table.writeInt(4 + payloadSize); table.writeInt(i + 1);
            table.write(payload.data(), payload.size());
        }
        check(packedBankFile.appendData(table.getData(), table.getDataSize()), "append actual bounded core table for native cancellation");
        client.openInitialFile(packedBankFile);
        if (bankScenario == "bank-open-cancel")
        { check(clickModal("Cancel BNK open"), "invoke actual native parser cancellation button"); next(610); }
        else if (bankScenario == "bank-open-edited")
        {
            check(document->moveEnvelopePoint(0, 0, 0.72f) && document->undo() && document->getState() == videoOriginalState,
                "real edit and Undo return the same State while replacing Redo during bank parsing");
            const auto changed = document->captureWorkspaceSnapshot();
            check(changed && !changed->sameContents(*bankWorkspace), "full workspace identity detects changed Redo");
            bankWorkspace = changed; next(610);
        }
        else if (bankScenario == "bank-open-close")
        { expectedQuit = true; client.requestCloseApplication(); next(450); }
        else if (bankScenario == "bank-open-queue")
        {
            queuedOpenBank = fixture.getChildFile("latest-native.bnk");
            juce::MemoryOutputStream latest;
            latest.write("BKHD", 4); latest.writeInt(8); latest.writeInt(128); latest.writeInt(99);
            check(queuedOpenBank.replaceWithData(latest.getData(), latest.getDataSize()), "create actual second native bank");
            client.openInitialFile(queuedOpenBank); next(615);
        }
        else check(false, "known native bank-open scenario");
    }
    static juce::ComboBox* bankSwitchControl(juce::Component& root)
    {
        if (auto* box = dynamic_cast<juce::ComboBox*>(&root); box && box->getNumItems() == 2
            && box->getItemText(0) == "Native A" && box->getItemText(1) == "Native B") return box;
        for (auto* child : root.getChildren()) if (auto* box = bankSwitchControl(*child)) return box;
        return nullptr;
    }
    static juce::ComboBox* bankStateControl(juce::Component& root)
    {
        if (auto* box = dynamic_cast<juce::ComboBox*>(&root); box &&
            ((box->getNumItems()==2 && box->getItemText(0)=="None" && box->getItemText(1)=="Rain")
             || (box->getNumItems()==4 && box->getItemText(0).contains("None") && box->getItemText(1).contains("Rain")
                 && box->getItemText(2).contains("None") && box->getItemText(3).contains("Rain")))) return box;
        for (auto* child : root.getChildren()) if (auto* box = bankStateControl(*child)) return box;
        return nullptr;
    }
    void prepareRealBank()
    {
        const auto manifest = juce::JSON::parse(realBankInput.getChildFile("manifest.json"));
        const auto inputs = realBankInput.getChildFile("fixtures");
        const auto copyInput = [&](const char* key)
        {
            const auto row = manifest.getProperty(key, {});
            const auto input = inputs.getChildFile(row.getProperty("path", {}).toString());
            check(input.isAChildOf(inputs) && input.existsAsFile()
                && juce::SHA256(input).toHexString().equalsIgnoreCase(row.getProperty("sha256", {}).toString()),
                "real native fixture matches its frozen manifest");
            const auto output = fixture.getChildFile("real-bank").getChildFile(input.getFileName());
            check(output.getParentDirectory().createDirectory().wasOk() && !output.exists() && input.copyFileTo(output),
                "copy only the authored bank and original sidecar into the product scan root");
            return output;
        };
        realBankFile = copyInput("bank"); copyInput("sidecar");
        const auto reference = manifest.getProperty("reference", {});
        realBankReference = inputs.getChildFile(reference.getProperty("path", {}).toString());
        check(realBankReference.isAChildOf(inputs) && realBankReference.existsAsFile()
            && juce::SHA256(realBankReference).toHexString().equalsIgnoreCase(reference.getProperty("sha256", {}).toString()),
            "independent direct bank/subsong reference remains outside the product input root");
        juce::MemoryBlock bytes;
        check(realBankFile.loadFileAsData(bytes) && bytes.getSize() == 3180370, "frozen genuine bank dimensions");
        realBankCorrupt = fixture.getChildFile("truncated-real-bank.bnk");
        check(realBankCorrupt.replaceWithData(bytes.getData(), 73), "derive one truncated core negative from the real bank bytes");
    }
    void captureRealBankOperation(MainComponent& client)
    {
        realBankWorker = observeNativeBnkWorker(client);
        realBankDiagnostic = observeNativeBnkDiagnostic(client);
        check(!realBankWorker.expired() && realBankDiagnostic, "actual Main starts a real bank worker with diagnostic capsule");
    }
    void checkRealBankDiagnostic(bool cancelled)
    {
        const auto report = realBankDiagnostic ? realBankDiagnostic->snapshot() : std::nullopt;
        check(report && report->terminal && (cancelled ? report->cancelled : report->succeeded),
            "real bank operation reaches its expected terminal outcome");
        if (report->process) check(report->process->jobCleanupConfirmed, "actual decoder process job is drained");
        if (!cancelled) check(report->process.has_value(), "positive real Wwise PCM uses the actual packaged decoder");
    }
    void waitRealBankPrepared(MainComponent& client, bool requiresFreshDecode = true)
    {
        // Hold only this test callback. The genuine background worker finishes,
        // while Main cannot consume its result before replacement or close.
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!nativeBnkWorkerFinished(client) && juce::Time::getMillisecondCounterHiRes() < deadline)
            juce::Thread::sleep(2);
        check(nativeBnkWorkerFinished(client), "genuine bank preparation finished before the deliberately late UI delivery");
        const auto report = realBankDiagnostic->snapshot();
        check(report && !report->terminal && (!requiresFreshDecode || report->process.has_value())
            && (!report->process || report->process->jobCleanupConfirmed),
            "genuine preparation drained before Main admission; a later cache hit need not launch another decoder");
    }
    void compareRealBankPcm()
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::FileInputStream(realBankReference), true));
        check(reader && reader->lengthInSamples == 176256 && reader->numChannels == 2 && reader->sampleRate == 44100.0,
            "real reference dimensions match the frozen direct bank decode");
        check(document->getFileInfo().file == realBankFile && document->getFileInfo().numSamples == reader->lengthInSamples
            && document->getFileInfo().numChannels == 2 && document->getFileInfo().sampleRate == reader->sampleRate,
            "actual native preview publishes complete real-bank audio");
        for (int64_t frame = 0; frame < reader->lengthInSamples; frame += 4096)
        {
            const auto count = static_cast<int>(juce::jmin<int64_t>(4096, reader->lengthInSamples - frame));
            juce::AudioBuffer<float> expected(2, count), actual(2, count);
            check(reader->read(&expected, 0, count, frame, true, true)
                && engine->renderDocumentBlockForDiagnostics(frame, actual), "render the complete genuine preview through AudioEngine");
            for (int channel = 0; channel < 2; ++channel) for (int i = 0; i < count; ++i)
                realBankMaxError = juce::jmax(realBankMaxError,
                    std::abs(static_cast<double>(expected.getSample(channel, i)) - actual.getSample(channel, i)));
            realBankFrames += count;
        }
        check(realBankMaxError == 0.0 && !engine->isPlaying(), "all native resolved samples equal the direct decoder oracle without physical playback");
    }
    void cleanupRealBank()
    {
        juce::MemoryBlock original;
        check(realBankFile.loadFileAsData(original), "read only the owned bank for exact extraction cleanup authority");
        const size_t offsets[] { 112, 705232, 1765168 }, lengths[] { 705120, 1059936, 1414752 };
        for (int i = 0; i < realBankExtracted.size(); ++i)
        {
            const auto input = realBankExtracted[i];
            if (!input.existsAsFile()) continue; // A cancelled request need not extract.
            check(juce::SHA256(input).toHexString() == juce::SHA256(static_cast<const char*>(original.getData()) + offsets[i], lengths[i]).toHexString(),
                "exact owned extraction still matches its genuine DIDX/DATA range");
            const auto wave = VgmstreamDecodeService::getDecodedWaveFile(input);
            const auto path = wave.getFullPathName().toLowerCase();
            juce::InterProcessLock lock("SingleEditor-vgmstream-" + juce::SHA256(path.toRawUTF8(), path.getNumBytesAsUTF8()).toHexString());
            check(lock.enter(5000) && wave.getParentDirectory() == VgmstreamDecodeService::getDecodeCacheDirectory(),
                "hold the exact owned decode cache lock after worker release");
            check(wave.deleteFile() && media::DecodedWaveCache::sealFile(wave).deleteFile()
                && wave.getSiblingFile(wave.getFileName() + ".decoding").deleteFile() && input.deleteFile(),
                "remove only this unique native fixture's extracted media and derived cache");
        }
        if (!realBankExtracted.isEmpty())
        {
            const auto directory = realBankExtracted[0].getParentDirectory();
            check(directory.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty() && directory.deleteFile(),
                "remove only the newly created empty bank extraction directory");
        }
        realBankChecks.add("exact-owned-cache-cleanup");
    }
    void runRealBankStep(MainComponent& client, double elapsed)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        if ((stage == 1 || stage == 7) && modal && modal->getName() == "BNK metadata unavailable")
        {
            check(bank->hasBank() && button(*modal, "Keep bank"), "original sidecar failure leaves the genuine bank core usable");
            if (stage == 1) { render(*modal, "real-sidecar-degraded.png"); realBankChecks.add("original-sidecar-degraded-with-usable-core"); }
            check(clickModal("Keep bank"), "acknowledge the genuine sidecar capability failure without fabricating event links");
            return;
        }
        switch (stage)
        {
            case 0:
                if (document->hasAudio() && waveform->isPeakCacheReady() && elapsed > 500.0)
                {
                    check(client.isCoreReadyForWorkspaceResources() && BnkRuntimeSession::factoryRequestsForTesting() == 0
                        && BnkRuntimeSession::liveInstancesForTesting() == 0,
                        "actual ordinary waveform and playable CoreReady require no BNK factory or instance");
                    realBankChecks.add("ordinary-core-ready-zero-bnk-factory-and-instance");
                    client.openInitialFile(realBankFile); next(1);
                }
                break;
            case 1:
                if (bank->hasBank() && !modal)
                {
                    check(bank->getState()->mediaEntries.size() == 3 && bank->getState()->events.size() == 3,
                        "actual Main parses all real embedded media and event descriptors");
                    realBankState = bank->getState();
                    check(BnkRuntimeSession::factoryRequestsForTesting() == 1 && BnkRuntimeSession::liveInstancesForTesting() == 1,
                        "one admitted real bank owns exactly one generation session");
                    realBankView = findComponent<BnkAudioListViewComponent>(*window);
                    check(realBankView != nullptr, "real bank creates its native media view");
                    for (const auto id : { uint32_t{39805429}, uint32_t{877403992}, uint32_t{996917016} })
                    {
                        const auto cache = bank->getEmbeddedMediaCacheFile(id);
                        check(cache != juce::File() && !cache.getParentDirectory().exists(), "real native extraction root is new and private");
                        realBankExtracted.add(cache);
                    }
                    activateBank(39805429); captureRealBankOperation(client); next(2);
                }
                break;
            case 2:
                if (!modal && document->getFileInfo().file == realBankFile && document->getFileInfo().numSamples == 176256)
                {
                    checkRealBankDiagnostic(false); compareRealBankPcm(); render(*window, "real-bank-complete.png");
                    realBankChecks.add("genuine-bank-native-full-pcm");
                    check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(),
                        "native real preview retains executable history before cancellation");
                    realBankWorkspace = document->captureWorkspaceSnapshot();
                    activateBank(877403992); captureRealBankOperation(client);
                    check(clickModal("Cancel BNK preview"), "click real native preview cancellation"); next(3);
                }
                break;
            case 3:
                if (!modal && nativeBnkWorkerFinished(client))
                {
                    const auto report = realBankDiagnostic->snapshot();
                    if (!report || !report->terminal) break;
                    checkRealBankDiagnostic(true);
                    check(document->captureWorkspaceSnapshot()->sameContents(*realBankWorkspace), "real cancellation preserves complete current/Undo/Redo workspace");
                    realBankChecks.add("native-cancel-preserves-history");
                    activateBank(996917016); captureRealBankOperation(client); waitRealBankPrepared(client);
                    client.openInitialFile(source); next(4);
                }
                break;
            case 4:
                if (!modal && document->getFileInfo().file == source && waveform->isPeakCacheReady() && elapsed > 500.0)
                {
                    checkRealBankDiagnostic(true);
                    check(!bank->hasBank() && realBankWorker.expired() && realBankView == nullptr && realBankState.expired(),
                        "ordinary generation releases real-bank parsed state, native view and completed stale worker");
                    check(BnkRuntimeSession::factoryRequestsForTesting() == 1 && BnkRuntimeSession::liveInstancesForTesting() == 0,
                        "ordinary replacement releases the BNK owner without requesting another factory");
                    realBankChecks.add("ordinary-replacement-zero-bnk-instance");
                    realBankWorkspace.reset();
                    realBankChecks.add("ordinary-generation-fences-completed-real-result");
                    render(*window, "ordinary-after-real-bank.png");
                    realBankWorkspace = document->captureWorkspaceSnapshot();
                    client.openInitialFile(realBankCorrupt); next(5);
                }
                break;
            case 5:
                if (modal && button(*modal, "Keep current audio"))
                {
                    check(!bank->hasBank() && document->captureWorkspaceSnapshot()->sameContents(*realBankWorkspace),
                        "truncated real bank rejects core admission and preserves ordinary workspace");
                    render(*modal, "truncated-real-bank.png");
                    check(clickModal("Keep current audio"), "dismiss real corrupt-bank failure");
                    realBankChecks.add("truncated-genuine-core-rejected"); next(6);
                }
                break;
            case 6:
                if (!modal) { realBankWorkspace.reset(); client.openInitialFile(realBankFile); next(7); }
                break;
            case 7:
                if (!modal && bank->hasBank())
                {
                    realBankCloseWorkspace = document->captureWorkspaceSnapshot();
                    if (!realBankCloseWorkspace)
                    {
                        check(!document->hasAudio(), "new real bank has an empty current audio document before preview");
                        realBankCloseEmptyState = document->getState();
                    }
                    activateBank(39805429); captureRealBankOperation(client); waitRealBankPrepared(client, false);
                    expectedQuit = true; client.requestCloseApplication(); next(8);
                }
                break;
            case 8: break;
        }
    }
    void activateBank(uint32_t id)
    {
        auto* list = findComponent<BnkAudioListViewComponent>(*window);
        check(list && list->onMediaActivated, "actual native BNK media activation callback exists");
        list->onMediaActivated(id);
        check(bank->getSelectedMediaId() == id, "actual native BNK selection follows requested media");
    }
    int bankInvocations(int id) const
    {
        auto lines = juce::StringArray::fromLines(fixture.getChildFile(juce::String(id) + ".wem.pids").loadFileAsString());
        lines.removeEmptyStrings(); return lines.size();
    }
    void finishBankInput(int id)
    {
        check(fixture.getChildFile(juce::String(id) + ".wem.finish").replaceWithText("finish"), "release only the owned real bank decoder gate");
    }
    void checkBankInputReleased()
    {
#if JUCE_WINDOWS
        // Ask for write access without writing: the request's real deny-write
        // input lease must be gone before cancellation/close is called settled.
        const auto input = fixture.getChildFile("100.wem");
        const auto handle = CreateFileW(input.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "native completed cancellation/close releases the exact physical input lease");
        CloseHandle(handle);
#endif
    }
    void checkBankWorkspace()
    {
        const auto current = document->captureWorkspaceSnapshot();
        check(current && bankWorkspace && current->sameContents(*bankWorkspace),
            "BNK pending/failure/cancel retains exact Document and complete Undo/Redo workspace");
        check(document->getState() == videoOriginalState && !document->hasUnsavedChanges() && !engine->isPlaying(),
            "BNK preparation does not replace current audio or start playback");
        checkPackedSelection();
        juce::AudioBuffer<float> pcm(2, 128);
        check(engine->renderDocumentBlockForDiagnostics(0, pcm), "real engine retains prior BNK PCM");
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
            check(pcm.getSample(c, i) == videoOriginalPcm.getSample(c, i), "BNK pending/failure does not change actual PCM");
    }
    void checkFallbackReturn(bool exerciseHistory = true)
    {
        const auto after = document->captureCommittedState(document->getMediaIdentity().document);
        check(fallbackHistory && after, "returned fallback has a complete real Document image");
        const auto& before = *fallbackHistory;
        check(!before.identity.document.sameDocument(after->identity.document)
            && !before.identity.source.sameBinding(after->identity.source)
            && !document->captureCommittedState(before.identity.document), "full-image reactivation mints fresh runtime/source identity and refuses old identity authority");
        const bool wasDirty = before.identity.savedGeneration != before.state->changeGeneration;
        check(after->sourceVersion == before.sourceVersion && document->hasUnsavedChanges() == wasDirty
            && (wasDirty || after->identity.savedGeneration == after->state->changeGeneration), "return preserves actual source witness and remaps the saved point honestly");
        const auto sameAudioAndEnvelope = [](const auto& a, const auto& b)
        {
            check(a && b && a->info.file == b->info.file && a->info.numSamples == b->info.numSamples
                && a->info.sampleRate == b->info.sampleRate && a->info.numChannels == b->info.numChannels
                && a->audioBuffer == b->audioBuffer && a->streamingReader == b->streamingReader
                && a->memoryMappedReader == b->memoryMappedReader && a->envelopePoints.size() == b->envelopePoints.size(),
                "every returned history state shares its real original backing reader/buffer and dimensions without loadFile");
            for (size_t i = 0; i < a->envelopePoints.size(); ++i)
                check(a->envelopePoints[i].timeSeconds == b->envelopePoints[i].timeSeconds && a->envelopePoints[i].gain == b->envelopePoints[i].gain,
                    "every original envelope point survives full history reactivation");
        };
        sameAudioAndEnvelope(before.state, after->state);
        check(before.undo.size() == after->undo.size() && before.redo.size() == after->redo.size(), "both complete history stacks retain their original lengths");
        for (size_t i = 0; i < before.undo.size(); ++i) sameAudioAndEnvelope(before.undo[i], after->undo[i]);
        for (size_t i = 0; i < before.redo.size(); ++i) sameAudioAndEnvelope(before.redo[i], after->redo[i]);
        if (exerciseHistory) for (const auto& expected : fallbackRedoPcm)
        {
            check(document->redo(), "execute every retained Redo after fallback");
            juce::AudioBuffer<float> pcm(2, 128);
            check(engine->renderDocumentBlockForDiagnostics(0, pcm), "render each actual reactivated Redo state");
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
                check(pcm.getSample(c, i) == expected.getSample(c, i), "every retained Redo produces identical real PCM before and after fallback");
        }
        if (exerciseHistory) for (size_t i = 0; i < fallbackRedoPcm.size(); ++i) check(document->undo(), "execute all actual Undo steps back to the saved point");
        check(document->hasUnsavedChanges() == wasDirty, "Undo returns to the true remapped saved/unsaved state");
        juce::AudioBuffer<float> currentPcm(2, 128);
        check(engine->renderDocumentBlockForDiagnostics(0, currentPcm), "returned current workspace renders actual PCM");
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
            check(currentPcm.getSample(c, i) == videoOriginalPcm.getSample(c, i), "current PCM is identical across full image reactivation");
        checkPackedSelection();
        videoOriginalState = document->getState(); bankWorkspace = document->captureWorkspaceSnapshot();
    }
    void checkCompleteBankPreview(uint32_t id, bool eventPreview = false)
    {
        check(document->getFileInfo().file == packedBankFile && document->getFileInfo().numSamples == 144000
            && document->getState()->audioBuffer && (eventPreview ? bank->getSelectedEventId() : bank->getSelectedMediaId()) == id && !engine->isPlaying(),
            "native BNK publishes only complete three-second PCM for the latest exact selection");
        juce::AudioBuffer<float> pcm(2, 128);
        check(engine->renderDocumentBlockForDiagnostics(96000, pcm), "real engine reads late frames of complete BNK preview");
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
            check(std::abs(pcm.getSample(c, i) - 0.25f * std::sqrt(0.5f)) < 1.0e-6f,
                "complete bank mono audio follows actual stereo channel routing at late frames");
    }
    static juce::ComboBox* locatorModeControl(juce::Component& root)
    {
        if (auto* box=dynamic_cast<juce::ComboBox*>(&root); box && box->getNumItems()==2
            && box->getItemText(0)=="Decode packed media (recommended)") return box;
        for (auto* child:root.getChildren()) if (auto* box=locatorModeControl(*child)) return box;
        return nullptr;
    }
    void showLocatorPreferences()
    {
        // Actual product control, with this test's AudioEngine and isolated
        // ApplicationProperties. Main's menu-opening route is not asserted here.
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle="Preferences";
        options.content.setOwned(new PreferencesComponent(*engine,properties));
        options.componentToCentreAround=window.get(); options.useNativeTitleBar=false;
        options.resizable=true; options.escapeKeyTriggersCloseButton=true;
        if (auto* dialog=options.launchAsync()) dialog->centreWithSize(920,860);
        else check(false,"open actual Preferences controls in owned native test window");
    }
    void checkLocatorNativePcm(float value)
    {
        const auto state=document->getState();
        check(state && state->audioBuffer && state->audioBuffer->getNumChannels()==1
            && state->audioBuffer->getNumSamples()==144000 && !engine->isPlaying(),"complete native locator PCM without autoplay");
        for (int i=0;i<144000;++i)
            check(std::abs(state->audioBuffer->getSample(0,i)-value)<1.0e-6f,"every native sample uses the selected actual locator role");
        juce::AudioBuffer<float> routed(2,128);
        check(engine->renderDocumentBlockForDiagnostics(96000,routed),"route actual late locator PCM");
        for (int c=0;c<2;++c) for (int i=0;i<128;++i)
            check(std::abs(routed.getSample(c,i)-value*std::sqrt(0.5f))<1.0e-6f,"locator role preserves real stereo routing");
    }
    void runBankNativeStep(MainComponent& client, double elapsed)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        switch (stage)
        {
            case 665:
                if (!modal && document->hasAudio() && document->getFileInfo().file == fallbackNewAudio && elapsed > 500.0)
                {
                    check(!bank->hasBank() && !document->canUndo() && !document->canRedo()
                        && playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == fallbackNewAudio,
                        "late disguised-bank result cannot replace a newer ordinary document/playlist row with the old history");
                    checkBankOpenInputsReleased();
                    juce::AudioBuffer<float> pcm(2, 128);
                    check(engine->renderDocumentBlockForDiagnostics(0, pcm), "newer ordinary input renders real PCM after superseding fallback");
                    for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
                        check(pcm.getSample(c, i) == videoOriginalPcm.getSample(c, i), "newer ordinary input retains its actual independent PCM");
                    render(*window, "bank-fallback-newer-ordinary.png"); expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 660:
                if (modal && modal->getName() == "BNK open unavailable")
                {
                    checkFallbackReturn(); checkBankOpenInputsReleased();
                    check(playlist->getCurrentEntry() && playlist->getCurrentEntry()->id == packedReturnedEntry
                        && playlist->getCurrentEntry()->recovered && document->getRecoveryOrigin().has_value()
                        && fallbackRecoveryOrigin && document->getRecoveryOrigin()->sameVersion(*fallbackRecoveryOrigin)
                        && document->requiresRecoverySaveAs() && document->hasUnsavedChanges(),
                        "actual failed disguised-bank activation returns the recovered owner row, origin and SaveAs guard with all history");
                    render(*window, "bank-fallback-recovered-history.png");
                    check(clickModal("Keep current audio"), "keep the actual recovered workspace after bank parser failure"); next(661);
                }
                break;
            case 661:
                if (!modal) { expectedQuit = true; client.requestCloseApplication(); next(75); }
                break;
            case 450:
                if (modal && modal->getName() == "Recovery still pending")
                {
                    check(bankCloseWaits++ == 0, "recovery close did not settle after one explicit Wait");
                    render(*modal, "bank-close-pending-wait.png");
                    std::cout << "Native bank close: explicit Wait after the existing 750 ms bound\n";
                    check(clickModal("Wait"), "wait for actual recovery settlement without forced exit or retry");
                }
                break;
            case 600:
                if (modal && modal->getName() == "BNK open unavailable")
                {
                    if (bankScenario.startsWith("bank-fallback-")) checkFallbackReturn();
                    checkBankWorkspace(); checkBankOpenInputsReleased();
                    check(playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == source,
                        "failed native bank open restores the actual previous playlist row");
                    check(button(*modal, "Retry once") && !button(*modal, "Retry with Expanded Budget"), "actual malformed core has one retry and no structural-limit expansion");
                    render(*modal, "bank-open-core-first-failure.png"); check(clickModal("Retry once"), "invoke actual core parser retry"); next(601);
                }
                break;
            case 601:
                if (modal && modal->getName() == "BNK open unavailable")
                {
                    checkBankWorkspace(); checkBankOpenInputsReleased();
                    check(!button(*modal, "Retry once") && componentText(*modal).contains("Two matching failures"), "actual second core failure opens native fuse");
                    render(*modal, "bank-open-core-fuse.png"); check(clickModal("Export diagnostics..."), "export actual core request diagnostic"); next(603);
                }
                break;
            case 603:
                if (auto* dialog = diagnosticsWindow(); dialog && !modal)
                {
                    auto* save = button(*dialog, "Save reviewed bundle...");
                    auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                    if (!save || !save->isEnabled() || !preview) break;
                    bankDiagnosticReview = preview->getText();
                    check(bankDiagnosticReview.contains("providers/codec.json") && !bankDiagnosticReview.contains("providers/audio.json")
                        && !bankDiagnosticReview.contains("providers/recovery.json") && button(*dialog, "Media")->getToggleState(),
                        "bank index/sidecar failure preselects the actual Media section");
                    render(*dialog, "bank-open-unified-review.png");
                    save->triggerClick(); next(606);
                }
                break;
            case 606:
                {
                    auto* dialog = diagnosticsWindow();
                    auto* content = dialog ? dynamic_cast<LocalDiagnosticsComponent*>(dialog->getContentComponent()) : nullptr;
                    check(content != nullptr, "existing native collector accepts explicit re-entry during a reviewed save");
                    if (!bankScopeQueued)
                    {
                        content->requestScope(std::set { diagnostics::ProviderKind::recovery });
                        content->requestScope(std::set { diagnostics::ProviderKind::audio });
                        check(button(*dialog, "Media")->getToggleState() && !button(*dialog, "Audio device")->getToggleState(),
                            "new scope requests leave the approved review and visible selection unchanged while the chooser is active");
                        bankScopeQueued = true;
                    }
                    if (chooseOwnedFile(artifacts.getChildFile("bank-open-diagnostics.zip"))) next(604);
                }
                break;
            case 604:
            {
                const auto file = artifacts.getChildFile("bank-open-diagnostics.zip");
                auto* dialog = diagnosticsWindow();
                auto* save = dialog ? button(*dialog, "Save reviewed bundle...") : nullptr;
                if (!file.existsAsFile() || modal || !save || !save->isEnabled()) break;
                check(static_cast<bool>(diagnostics::DiagnosticBundle::validate(file).status), "bank failure entry uses the actual unified reviewed bundle");
                juce::ZipFile archive(file);
                const auto index = archive.getIndexOfFileName("providers/codec.json");
                check(index >= 0, "unified bank failure includes loaded codec provider");
                auto stream = archive.createStreamForEntry(index);
                check(stream != nullptr, "read actual codec section from native bank export");
                const auto text = stream->readEntireStreamAsString(); const auto value = juce::JSON::parse(text);
                const auto fields = value["fields"];
                const auto* ids = value["operationIds"].getArray();
                const bool metadata = bankScenario == "bank-open-sidecar" || bankScenario == "bank-open-derived";
                check(!text.contains(fixture.getFullPathName()) && !text.contains(packedBankFile.getFileName())
                    && fields[metadata ? "bankSidecarCircuit" : "bankCoreCircuit"]["value"].toString() == "open"
                    && bankDiagnosticReview.contains(juce::JSON::toString(value, false)),
                    "native failure export matches the preview, capability fuse and default identifier exclusions");
                check(fields["codecFailureSource"]["value"].toString() == (metadata ? "bank-sidecar" : "bank-index")
                    && !juce::Uuid(value["latestFailure"]["operationId"].toString()).isNull()
                    && ids && ids->contains(value["latestFailure"]["operationId"]),
                    "actual parser capsule is correlated with its request rather than an earlier decoder");
                const auto* refreshed = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                check(refreshed && refreshed->getText() != bankDiagnosticReview && refreshed->getText().contains("providers/audio.json")
                    && !refreshed->getText().contains("providers/codec.json") && !refreshed->getText().contains("providers/recovery.json")
                    && archive.getIndexOfFileName("providers/audio.json") < 0,
                    "last queued scope wins only after the originally reviewed Media snapshot publishes unchanged");
                dialog->closeButtonPressed();
                if (metadata)
                {
                    check(fields["bankCoreState"]["value"].toString() == "published", "sidecar fuse does not fail its independently adopted core");
                    if (bankScenario == "bank-open-derived")
                    {
                        check(static_cast<int>(fields["codecFailureDetail"]["value"]) == static_cast<int>(BnkParser::Failure::hardLimit),
                            "actual native capsule records structural derived-text limit, not timeout or malformed input");
                        for (int i = 0; i < 4; ++i)
                            check(fixture.getChildFile("Actor-Mixer Hierarchy").getChildFile(juce::String(i) + ".wwu").replaceWithText("<WwiseDocument/>"),
                                "repair only owned authoring inputs after actual request leases drain");
                    }
                    check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText(
                        "<SoundBanksInfo><SoundBanks><SoundBank><Path>native-preview.bnk</Path><IncludedEvents><Event Id=\"909\" Name=\"Repaired\"/></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),
                        "repair only the actual native metadata input");
                }
                else createNativeBank();
                client.openInitialFile(packedBankFile); next(605); break;
            }
            case 605:
                if (!modal && bank->hasBank())
                {
                    if (bankScenario == "bank-open-sidecar" || bankScenario == "bank-open-derived") check(bank->getState()->events.size() == 1 && bank->getState()->events[0].id == 909,
                        "changed native sidecar identity opens fresh and publishes its actual repaired event");
                    checkBankOpenInputsReleased(); render(*window, "bank-open-repaired.png");
                    if (bankScenario == "bank-open-sidecar") { activateBank(99); next(630); break; }
                    if (bankScenario == "bank-open-derived")
                    {
                        auto* tab = button(*window, "Audio"); check(tab, "actual repaired bank Audio tab is available");
                        tab->triggerClick(); next(624); break;
                    }
                    expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 610:
                if (!modal && elapsed > 100.0 && bankOpenInputsReleased())
                {
                    // The worker releases its file lease before MainComponent's
                    // next timer consumes the result and restores the row. Modal
                    // dismissal is asynchronous too, so neither proves UI completion.
                    // Keep the same final assertions, with a bounded observable wait.
                    if ((!playlist->getCurrentEntry() || playlist->getCurrentEntry()->file != source)
                        && elapsed < 5000.0) break;
                    checkBankWorkspace(); checkBankOpenInputsReleased(); check(!bank->hasBank(), "cancelled/stale native bank result never publishes");
                    check(playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == source,
                        "native cancellation retains the actual original playlist row");
                    render(*window, "bank-open-cancelled-history.png"); expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 615:
                if (!modal && bank->hasBank())
                {
                    check(bank->getBankInfo().file == queuedOpenBank && bank->getBankInfo().bankId == 99,
                        "native bank queue publishes only its latest actual bank source");
                    checkBankOpenInputsReleased(); render(*window, "bank-open-latest-queue.png");
                    expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 620:
                if (modal && modal->getName() == "BNK metadata unavailable")
                {
                    check(bank->hasBank() && bank->getState()->events.empty() && bank->getState()->mediaEntries.size() == 4,
                        "actual native sidecar failure keeps the independently parsed complete core");
                    check(!button(*modal, "Retry with Expanded Budget"), "fixed metadata limits never offer a wall-time expansion bypass");
                    checkBankOpenInputsReleased(); render(*modal, "bank-open-sidecar-first-failure.png");
                    check(clickModal("Retry once"), "invoke the actual sidecar-only retry"); next(621);
                }
                break;
            case 621:
                if (modal && modal->getName() == "BNK metadata unavailable")
                {
                    check(bank->hasBank() && !button(*modal, "Retry once") && componentText(*modal).contains("Two matching failures"),
                        "native metadata retry reaches its own fuse without disabling the core bank");
                    checkBankOpenInputsReleased(); render(*modal, "bank-open-sidecar-fuse.png");
                    check(clickModal("Export diagnostics..."), "export actual sidecar request diagnostic"); next(603);
                }
                break;
            case 624:
                if (auto* audio = findComponent<BnkAudioListViewComponent>(*window); !modal && audio && audio->isShowing())
                { bankScopePriorState = document->getState(); activateBank(99); next(625); }
                break;
            case 625:
                if (!modal && document->getState() != bankScopePriorState && document->getFileInfo().file == packedBankFile
                    && document->getFileInfo().numSamples == 48000)
                {
                    check(bank->getSelectedMediaId() == uint32_t{99} && bank->hasBank(), "repaired native bank publishes its actual selected embedded preview");
                    juce::AudioBuffer<float> pcm(2, 128);
                    check(engine->renderDocumentBlockForDiagnostics(0, pcm), "render actual repaired bank preview");
                    for (int c = 0; c < pcm.getNumChannels(); ++c) for (int i = 0; i < pcm.getNumSamples(); ++i)
                        check(std::abs(pcm.getSample(c, i) - videoOriginalPcm.getSample(c, i)) < 1.0e-6f,
                            "derived-path failure/retry/repair retains the original embedded audio samples");
                    check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->redo() && document->undo(),
                        "actual repaired preview retains editable Undo and Redo");
                    checkBankOpenInputsReleased(); render(*window, "bank-open-derived-preview.png"); bankScopePriorState.reset();
                    expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 630:
                if (!modal && document->getState()->audioBuffer && document->getFileInfo().numSamples == 48000)
                {
                    packedBankCache = bank->getEmbeddedMediaCacheFile(99);
                    packedBankCacheHash = juce::SHA256(packedBankCache).toHexString();
                    check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(),
                        "attach begins with actual BNK preview and retained Redo");
                    videoOriginalState = document->getState();
                    check(engine->renderDocumentBlockForDiagnostics(0, videoOriginalPcm), "capture actual BNK PCM before metadata re-read");
                    retainPackedSelection(); bankWorkspace = document->captureWorkspaceSnapshot();
                    check(fixture.getChildFile("SoundbanksInfo.xml").replaceWithText("<!DOCTYPE SoundBanksInfo><SoundBanksInfo/>"),
                        "create new actual metadata failure and owned project root for Attach Project");
                    auto* attach = button(*window, "Attach Project");
                    check(attach && attach->isEnabled(), "actual Attach Project button is available");
                    attach->triggerClick(); next(631);
                }
                break;
            case 631:
                if (chooseOwnedInitialProjectDirectory()) next(632);
                break;
            case 632:
                if (modal && modal->getName() == "BNK metadata unavailable")
                {
                    checkBankWorkspace(); checkBankOpenInputsReleased();
                    WorkspaceSessionSupport settingsView(properties, *document, *bank, *playlist, *waveform);
                    check(settingsView.getMappedBnkProjectRoot(packedBankFile) == fixture,
                        "native directory chooser persisted exactly the owned bank parent in the real session settings");
                    check(bank->getState()->projectRoot == juce::File(), "failed metadata transaction does not publish a partial project root");
                    check(bank->getSelectedMediaId() == uint32_t{99}, "actual Attach Project retains active preview media selection");
                    render(*modal, "bank-attach-live-failure.png");
                    check(clickModal("Retry once"), "retry only failed attached metadata while the preview stays live"); next(633);
                }
                break;
            case 633:
                if (modal && modal->getName() == "BNK metadata unavailable")
                {
                    checkBankWorkspace(); checkBankOpenInputsReleased();
                    check(bank->getSelectedMediaId() == uint32_t{99} && !button(*modal, "Retry once")
                        && componentText(*modal).contains("Two matching failures"), "attached metadata fuse preserves complete live preview workspace");
                    render(*modal, "bank-attach-live-fuse.png");
                    check(clickModal("Keep bank"), "dismiss attached metadata failure without replacing audio"); next(634);
                }
                break;
            case 634:
                if (!modal)
                {
                    check(document->redo() && std::abs(document->getState()->envelopePoints.front().gain - 0.43f) < 0.001f && document->undo(),
                        "actual retained preview Redo and Undo execute after attached metadata fuse");
                    checkBankWorkspace(); render(*window, "bank-attach-live-history.png");
                    expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 400:
                if (bank->hasBank() && !modal) { activateBank(99); next(401); }
                break;
            case 401:
                if (!modal && document->getState()->audioBuffer && document->getFileInfo().numSamples == 48000)
                {
                    packedBankCache = bank->getEmbeddedMediaCacheFile(99);
                    packedBankCacheHash = juce::SHA256(packedBankCache).toHexString();
                    check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(),
                        "real bank Document starts with clean current state and retained Redo");
                    videoOriginalState = document->getState();
                    check(engine->renderDocumentBlockForDiagnostics(0, videoOriginalPcm), "capture actual original bank PCM");
                    retainPackedSelection(); bankWorkspace = document->captureWorkspaceSnapshot();
                    check(bankWorkspace != nullptr, "capture complete bank workspace baseline");
                    if (bankScenario == "bank-event" || bankScenario == "bank-switch")
                    { activateBankEvent(bankScenario == "bank-event" ? 201 : 202); next(bankScenario == "bank-event" ? 500 : 510); }
                    else if (bankScenario == "bank-state-scope") { activateBankEvent(203); next(550); }
                    else if (bankScenario == "bank-rtpc-metadata") { activateBankEvent(203); next(560); }
                    else if (bankScenario == "bank-media-locators") { activateBankEvent(203); next(570); }
                    else { activateBank(100); next(402); }
                }
                break;
            case 402:
                if (bankInvocations(100) == 1)
                {
                    check(modal && modal->getName() == "Preparing BNK preview", "actual BNK progress remains responsive while decoder is running");
                    checkBankWorkspace();
                    if (bankScenario == "bank-close")
                    { expectedQuit = true; client.requestCloseApplication(); next(450); break; }
                    if (bankScenario == "bank-close-cancel")
                    {
                        check(document->moveEnvelopePoint(0, 0, 0.72f), "make real edit during bank preparation before reviewing close");
                        bankWorkspace = document->captureWorkspaceSnapshot(); finishBankInput(100);
                        // Hold this native UI callback until actual preparation
                        // releases its input; Main's timer cannot consume the
                        // completion before the close command below.
                        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
                        bool released = false;
#if JUCE_WINDOWS
                        const auto input = fixture.getChildFile("100.wem");
                        while (juce::Time::getMillisecondCounterHiRes() < deadline)
                        {
                            const auto handle = CreateFileW(input.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); released = true; break; }
                            juce::Thread::sleep(5);
                        }
#endif
                        check(released, "actual bank worker finishes preparation while native result delivery is pending");
                        juce::Thread::sleep(20); checkVideoChildrenExited();
                        client.requestCloseApplication(); next(520); break;
                    }
                    if (bankScenario == "bank-cancel")
                    { check(clickModal("Cancel BNK preview"), "click actual BNK cancellation"); next(430); break; }
                    if (bankScenario == "bank-edited")
                    {
                        const auto previous = document->getState();
                        check(document->moveEnvelopePoint(0, 0, 0.72f) && document->undo() && document->getState() == previous,
                            "edit and Undo return exact prior State while changing actual Redo history");
                        const auto changed = document->captureWorkspaceSnapshot();
                        check(changed && !changed->sameContents(*bankWorkspace), "full workspace fence sees changed Redo despite identical State");
                        bankWorkspace = changed; finishBankInput(100); next(431); break;
                    }
                    if (bankScenario == "bank-queue")
                    { activateBank(101); activateBank(102); next(432); break; }
                    if (bankScenario == "bank-replaced")
                    { client.openInitialFile(source); next(434); break; }
                    finishBankInput(100); next(bankScenario == "bank-budget" ? 420 : 403);
                }
                break;
            case 403:
                if (modal && modal->getName() == "BNK preview unavailable")
                {
                    checkBankWorkspace(); checkVideoChildrenExited(); checkBankInputReleased();
                    check(bankInvocations(100) == 1 && button(*modal, "Retry once") && !button(*modal, "Retry with Expanded Budget"),
                        "first actual decoder exit exposes one explicit retry without quota expansion");
                    render(*modal, "bank-first-failure.png");
                    check(clickModal("Retry once"), "click actual BNK Retry once"); next(404);
                }
                break;
            case 404:
                if (modal && modal->getName() == "BNK preview unavailable" && bankInvocations(100) == 2)
                {
                    checkBankWorkspace(); checkVideoChildrenExited();
                    check(!button(*modal, "Retry once"), "second matching BNK failure opens session fuse");
                    render(*modal, "bank-session-fused.png");
                    check(clickModal("Keep current audio"), "dismiss fused bank preview"); next(405);
                }
                break;
            case 405:
                if (!modal) { activateBank(100); next(406); }
                break;
            case 406:
                if (modal && modal->getName() == "BNK preview unavailable")
                {
                    check(bankInvocations(100) == 2 && !button(*modal, "Retry once"), "new native activation cannot bypass same bank scope fuse");
                    checkBankWorkspace();
                    check(clickModal("Export diagnostics..."), "explicit native local BNK diagnostic export"); next(407);
                }
                break;
            case 407:
                if (chooseReviewedMediaDiagnosticFile("bank-diagnostics")) next(408);
                break;
            case 408:
            {
                if (!finishReviewedMediaDiagnosticFile("bank-diagnostics")) break;
                next(440); break;
            }
            case 420:
                if (modal && modal->getName() == "BNK preview unavailable")
                {
                    checkBankWorkspace(); checkVideoChildrenExited();
                    check(bankInvocations(100) == 1 && button(*modal, "Retry with Expanded Budget")
                        && modal->getDescription().contains("4 GiB"), "real sparse quota failure explains finite expansion before explicit retry");
                    render(*modal, "bank-budget-review.png");
                    check(clickModal("Retry with Expanded Budget"), "click actual bank budget expansion"); next(421);
                }
                break;
            case 421:
                if (!modal && document->getFileInfo().numSamples == 144000)
                { check(bankInvocations(100) == 2, "bank budget expands once after real quota failure"); checkCompleteBankPreview(100); next(440); }
                break;
            case 430:
                if (!modal && elapsed > 100.0)
                { checkBankWorkspace(); checkVideoChildrenExited(); checkBankInputReleased(); next(440); }
                break;
            case 431:
                if (!modal && elapsed > 100.0)
                {
                    checkBankWorkspace(); checkVideoChildrenExited(); checkBankInputReleased();
                    check(document->redo() && std::abs(document->getState()->envelopePoints.front().gain - 0.72f) < 0.001f
                        && document->undo(), "new real Redo/Undo remains executable after stale successful bank completion");
                    checkBankWorkspace(); render(*window, "bank-edited-history-retained.png"); next(440);
                }
                break;
            case 432:
                if (bankInvocations(102) == 1)
                {
                    check(bankInvocations(100) == 1 && bankInvocations(101) == 0 && videoInvocations() == 2,
                        "actual queue coalesces intermediate selection without launching its child");
#if JUCE_WINDOWS
                    auto first = juce::StringArray::fromLines(fixture.getChildFile("100.wem.pids").loadFileAsString());
                    check(!first.isEmpty() && singleeditor::tests::decoderProcessExited(first[0]),
                        "previous bank decoder exits before latest queued child starts");
#endif
                    checkBankWorkspace(); check(bank->getSelectedMediaId() == uint32_t{102}, "latest native bank selection survives old work completion");
                    finishBankInput(102); next(433);
                }
                break;
            case 433:
                if (!modal && document->getFileInfo().numSamples == 144000)
                { checkCompleteBankPreview(102); next(440); }
                break;
            case 434:
                if (!modal && document->getFileInfo().file == source && waveform->isPeakCacheReady())
                {
                    check(!bank->hasBank() && bank->getBankInfo().file == juce::File() && videoInvocations() == 1,
                        "ordinary audio replaces pending BNK, clears parsed bank and causes no hidden retry");
                    check(diagnosticChild(*window,"bank.metadata.viewport") == nullptr,
                        "returning to ordinary audio releases the bank metadata viewport and its rows");
                    checkVideoChildrenExited(); checkBankInputReleased(); next(440);
                }
                break;
            case 500:
                if (modal && modal->getName() == "BNK preview unavailable")
                {
                    check(videoInvocations() == 1 && bank->getSelectedEventId() == uint32_t{201},
                        "native random event uses exactly one actual decoder candidate");
                    bankChosenInput = bankInvocations(100) == 1 ? 100 : 101;
                    checkBankWorkspace(); checkVideoChildrenExited();
                    check(clickModal("Retry once"), "retry the actual frozen native event request"); next(501);
                }
                break;
            case 501:
                if (modal && modal->getName() == "BNK preview unavailable" && videoInvocations() == 2)
                {
                    check(bankInvocations(bankChosenInput) == 2 && bankInvocations(bankChosenInput == 100 ? 101 : 100) == 0
                        && !button(*modal, "Retry once"), "native random retry retains the exact member and reaches its matching fuse");
                    checkBankWorkspace(); checkVideoChildrenExited(); render(*modal, "bank-event-fixed-retry.png");
                    check(clickModal("Keep current audio"), "retain current after native event fuse"); next(440);
                }
                break;
            case 510:
                if (bankInvocations(100) == 1)
                {
                    checkBankWorkspace();
                    auto* box = bankSwitchControl(*window);
                    check(box && box->getSelectedItemIndex() == 0, "actual native switch defaults to sidecar Native A");
                    box->setSelectedItemIndex(1, juce::sendNotificationSync); next(511);
                }
                break;
            case 511:
                if (bankInvocations(101) == 1)
                {
                    check(bankInvocations(100) == 1 && videoInvocations() == 2, "actual native switch change admits only the new selected member");
                    checkBankWorkspace(); finishBankInput(101); next(512);
                }
                break;
            case 512:
                if (!modal && document->getFileInfo().numSamples == 144000)
                {
                    auto* box = bankSwitchControl(*window);
                    check(box && box->getSelectedItemIndex() == 1, "late prior switch request cannot revert actual Native B control");
                    checkCompleteBankPreview(202, true); checkVideoChildrenExited(); render(*window, "bank-switch-complete.png"); next(440);
                }
                break;
            case 570:
                if (!modal && document->getFileInfo().numSamples==144000)
                {
                    checkCompleteBankPreview(203,true); checkLocatorNativePcm(0.25f);
                    const auto media=bank->getPlayableMediaById(100);
                    check(media && media->sourceFile==fixture.getChildFile("Originals/exact.wav")
                        && media->mediaFile==fixture.getChildFile("100.wem"),"native Audio descriptor retains exact original and generated roles");
                    render(*window,"bank-locator-packed.png"); showLocatorPreferences(); next(571);
                }
                break;
            case 571:
            case 575:
                if (modal && modal->getName()=="Preferences")
                {
                    auto* box=locatorModeControl(*modal);
                    check(box && box->isShowing(),"actual BNK playback mode control is visible");
                    const auto point=modal->getLocalPoint(box,box->getLocalBounds().getCentre());
                    auto* hit=modal->getComponentAt(point);
                    check(hit && (hit==box || box->isParentOf(hit)),"actual playback mode control is hit-testable");
                    box->grabKeyboardFocus();
                    check(box->keyPressed(juce::KeyPress(stage==571?juce::KeyPress::downKey:juce::KeyPress::upKey)),
                        "real Preferences key handler changes the existing playback mode");
                    check(box->getSelectedId()==(stage==571?2:1),"actual mode control selected the requested item");
                    next(stage==571?578:579); // JUCE delivers ComboBox onChange asynchronously.
                }
                break;
            case 578:
            case 579:
                if (modal && modal->getName()=="Preferences"
                    && properties.getUserSettings()->getIntValue(PreferencesComponent::bnkPlaybackModeKey)==(stage==578?2:1))
                {
                    auto* box=locatorModeControl(*modal);
                    check(box && box->getSelectedId()==(stage==578?2:1),"actual preference callback persisted the selected mode");
                    render(*modal,stage==578?"bank-locator-prefer-source.png":"bank-locator-prefer-packed.png");
                    bankScopePriorState=document->getState(); modal->exitModalState(0); next(stage==578?572:576);
                }
                break;
            case 572:
                if (!modal) { activateBankEvent(203); next(573); }
                break;
            case 576:
                if (!modal)
                {
                    auto* tab=button(*window,"Events"); check(tab,"actual Events tab is available");
                    tab->triggerClick(); next(581);
                }
                break;
            case 581:
                if (auto* events=findComponent<BnkEventListViewComponent>(*window); !modal && events && events->isShowing())
                { bankScopePriorState=document->getState(); activateBankEvent(203); next(577); }
                break;
            case 573:
                if (!modal && document->getState()!=bankScopePriorState && document->getFileInfo().numSamples==144000)
                {
                    checkLocatorNativePcm(0.5f); render(*window,"bank-locator-source-event.png");
                    auto* tab=button(*window,"Audio"); check(tab,"actual Audio tab is available");
                    tab->triggerClick(); next(580);
                }
                break;
            case 580:
                if (auto* audio=findComponent<BnkAudioListViewComponent>(*window); !modal && audio && audio->isShowing())
                { bankScopePriorState=document->getState(); activateBank(100); next(574); }
                break;
            case 574:
                if (!modal && document->getState()!=bankScopePriorState && document->getFileInfo().numSamples==144000)
                {
                    checkLocatorNativePcm(0.5f); check(bank->getSelectedMediaId()==uint32_t{100},"actual raw Audio activation uses original mode");
                    render(*window,"bank-locator-source-audio.png"); showLocatorPreferences(); next(575);
                }
                break;
            case 577:
                if (!modal && document->getState()!=bankScopePriorState && document->getFileInfo().numSamples==144000)
                {
                    checkCompleteBankPreview(203,true); checkLocatorNativePcm(0.25f);
                    check(bankInvocations(100)==1,"native mode switch back reuses only exact validated generated decoder output");
                    render(*window,"bank-locator-return-packed.png"); bankScopePriorState.reset();
                    std::cout << "Native media locators passed (actual preference controls, source/packed event and raw activation, complete distinct PCM and one decoder)\n";
                    next(440);
                }
                break;
            case 560:
                if (!modal && document->getFileInfo().numSamples==144000)
                {
                    checkCompleteBankPreview(203,true);
                    const auto parameters=bank->getAvailableRtpcs(); const auto state=bank->getState();
                    check(parameters.size()==1 && parameters[0].hasDefinition && parameters[0].token=="rtpc-right"
                        && parameters[0].maxValue==20 && parameters[0].bindings.size()==1
                        && parameters[0].bindings[0].objectId=="target-a" && parameters[0].bindings[0].points[0].y==-30,
                        "native parameter and curve use explicit GUID and exact object, with no invented RTPC audio effect");
                    check(state->rtpcs.size()==2 && state->rtpcs[0].bindings.empty()
                        && BnkDocument::hasRtpcMetadataForEvent(*state,state->events[0])
                        && !BnkDocument::hasRtpcMetadataForEvent(*state,state->events[1])
                        && !BnkDocument::hasRtpcMetadataForEvent(*state,state->events[2]),"native row metadata marks only the proven RTPC event despite equal target names");
                    auto* viewport = dynamic_cast<juce::Viewport*>(diagnosticChild(*window,"bank.metadata.viewport"));
                    check(viewport && viewport->keyPressed(juce::KeyPress(juce::KeyPress::pageDownKey)),
                        "actual bank metadata viewport accepts Page Down");
                    next(559); // ScrollBar publishes keyboard movement asynchronously.
                }
                break;
            case 559:
                if (auto* viewport = dynamic_cast<juce::Viewport*>(diagnosticChild(*window,"bank.metadata.viewport"));
                    viewport && viewport->getViewPositionY() > 0)
                {
                    check(viewport->getViewArea().getY() > 0,
                        "Page Down reaches actual scrolled bank content after message delivery");
                    viewport->setViewPosition(0,0);
                    render(*window,"bank-rtpc-exact-event.png");
                    auto* metadata=findComponent<MetadataSidebarComponent>(*window);
                    check(metadata!=nullptr,"actual metadata sidebar exists for RTPC inspection");
                    render(*metadata,"bank-rtpc-exact-metadata-top.png");
                    checkBankMetadataLayout(true,"bank-rtpc-exact-metadata-scrolled.png");
                    const auto metadataBounds=metadata->getBounds();
                    metadata->setSize(600,2200);
                    render(*metadata,"bank-rtpc-exact-metadata-expanded.png");
                    metadata->setBounds(metadataBounds);
                    const auto windowBounds = window->getBounds();
                    window->setBounds(windowBounds.withSizeKeepingCentre(920,680));
                    checkBankMetadataLayout(true,"bank-rtpc-exact-metadata-narrow.png");
                    window->setBounds(windowBounds);
                    auto* night = button(*window,"appearance.theme.night");
                    auto* light = button(*window,"appearance.theme.light");
                    check(night && night->onClick && light && light->onClick,"actual theme actions remain available");
                    night->onClick();
                    check(RendererVisualStyle::isNight(),"native Night action switches the existing metadata theme");
                    checkBankMetadataLayout(true,"bank-rtpc-exact-metadata-night.png");
                    light->onClick();
                    checkCompleteBankPreview(203,true);
                    bankScopePriorState=document->getState();
                    activateBankEvent(205); next(561);
                }
                break;
            case 561:
                if (!modal && document->getState()!=bankScopePriorState && document->getFileInfo().numSamples==144000)
                {
                    checkCompleteBankPreview(205,true);
                    check(bank->getAvailableRtpcs().empty(),"native same-name other target has no copied global RTPC controls");
                    check(bankInvocations(100)==1,"RTPC metadata switching reuses the validated actual decoder output");
                    render(*window,"bank-rtpc-other-event.png");
                    auto* metadata=findComponent<MetadataSidebarComponent>(*window);
                    check(metadata!=nullptr,"actual metadata sidebar remains available after RTPC target switch");
                    checkBankMetadataLayout(false,"bank-rtpc-other-metadata-scrolled.png");
                    const auto metadataBounds=metadata->getBounds();
                    metadata->setSize(600,2200);
                    render(*metadata,"bank-rtpc-other-metadata-expanded.png");
                    metadata->setBounds(metadataBounds);
                    bankScopePriorState.reset();
                    std::cout << "Native RTPC metadata passed (actual same-name parameter/object selection, truthful event badges, complete unchanged PCM and no RTPC effect)\n";
                    next(440);
                }
                break;
            case 550:
                if (!modal && document->getFileInfo().numSamples == 144000)
                {
                    checkCompleteBankPreview(203, true);
                    auto* box = bankStateControl(*window);
                    check(box && box->isShowing() && box->getNumItems()==4 && box->getSelectedItemIndex() == 2,
                        "actual native State control retains both groups and defaults to exact target None");
                    check(box->getItemText(1)!=box->getItemText(3) && box->getItemText(1).contains("group-shadow")
                        && box->getItemText(3).contains("group-weather"), "same-name group options have distinct reviewable labels");
                    const auto controlPoint = window->getLocalPoint(box, box->getLocalBounds().getCentre());
                    auto* hit = window->getComponentAt(controlPoint);
                    check(hit && (hit == box || box->isParentOf(hit)), "actual State control is visible and hit-testable above the bank canvas");
                    const auto previousBounds = window->getBounds();
                    window->setSize(920, 680);
                    const auto narrowPoint = window->getLocalPoint(box, box->getLocalBounds().getCentre());
                    auto* narrowHit = window->getComponentAt(narrowPoint);
                    check(box->getWidth() >= 120 && box->getHeight() >= 24 && narrowHit
                        && (narrowHit == box || box->isParentOf(narrowHit)), "narrow General layout keeps State controls large enough and hit-testable");
                    auto* attach = button(*window, "Attach Project");
                    check(attach, "existing Attach Project remains available beside the narrow bank controls");
                    auto* attachHit = window->getComponentAt(window->getLocalPoint(attach, attach->getLocalBounds().getCentre()));
                    check(attachHit && (attachHit == attach || attach->isParentOf(attachHit)), "narrow bank header does not overlap Attach Project");
                    render(*window, "bank-state-narrow-layout.png");
                    window->setBounds(previousBounds);
                    bankScopePriorState = document->getState();
                    box->grabKeyboardFocus();
                    check(box->hasKeyboardFocus(true) && box->keyPressed(juce::KeyPress(juce::KeyPress::downKey)),
                        "focused native State control handles the real Down-key selection path");
                    next(551);
                }
                break;
            case 551:
                if (!modal && document->getState() != bankScopePriorState && document->getFileInfo().numSamples == 144000)
                {
                    const auto current = document->getState();
                    check(current->audioBuffer && bankScopePriorState->audioBuffer && !engine->isPlaying(), "actual State callback publishes complete PCM without starting playback");
                    const auto gain = std::pow(10.0f, -6.0f / 20.0f);
                    check(current->audioBuffer->getNumChannels() == bankScopePriorState->audioBuffer->getNumChannels(), "state gain preserves real channel count");
                    for (int c = 0; c < current->audioBuffer->getNumChannels(); ++c)
                        for (int i = 0; i < 144000; ++i)
                            check(std::abs(current->audioBuffer->getSample(c, i) - bankScopePriorState->audioBuffer->getSample(c, i) * gain) < 1.0e-6f,
                                "every actual native sample follows target-a -6dB despite a colliding object name");
                    juce::AudioBuffer<float> pcm(2, 128);
                    check(engine->renderDocumentBlockForDiagnostics(96000, pcm), "native engine reads State-adjusted late PCM");
                    for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
                        check(std::abs(pcm.getSample(c, i) - 0.25f * gain * std::sqrt(0.5f)) < 1.0e-6f, "actual State gain preserves stereo routing");
                    check(bankInvocations(100) == 1, "State change reuses its validated actual decoded media");
                    render(*window, "bank-state-exact-target.png"); bankScopePriorState = current;
                    auto* box=bankStateControl(*window); check(box,"State control remains after first group gain");
                    box->grabKeyboardFocus();
                    for (int step=0;step<3;++step)
                        check(box->keyPressed(juce::KeyPress(juce::KeyPress::upKey)),"native Up reaches the other group's None");
                    next(553);
                }
                break;
            case 553:
                if (!modal && document->getState()!=bankScopePriorState && document->getFileInfo().numSamples==144000)
                {
                    checkCompleteBankPreview(203,true);
                    auto* box=bankStateControl(*window); check(box && box->getSelectedItemIndex()==0,"other group None is current");
                    bankScopePriorState=document->getState(); box->grabKeyboardFocus();
                    check(box->keyPressed(juce::KeyPress(juce::KeyPress::downKey)),"native Down selects same-name Rain in the other group"); next(554);
                }
                break;
            case 554:
                if (!modal && document->getState()!=bankScopePriorState && document->getFileInfo().numSamples==144000)
                {
                    const auto current=document->getState(); const auto gain=std::pow(10.0f,3.0f/20.0f);
                    auto* box=bankStateControl(*window); check(box && box->getSelectedItemIndex()==1 && !engine->isPlaying(),"other group's Rain is selected without autoplay");
                    check(current->audioBuffer && current->audioBuffer->getNumChannels()==bankScopePriorState->audioBuffer->getNumChannels(),"other State preserves channels");
                    for (int c=0;c<current->audioBuffer->getNumChannels();++c) for (int i=0;i<144000;++i)
                        check(std::abs(current->audioBuffer->getSample(c,i)-0.25f*gain)<1.0e-6f,"every PCM sample uses only the other group's +3dB");
                    juce::AudioBuffer<float> routed(2,128); check(engine->renderDocumentBlockForDiagnostics(96000,routed),"route other group PCM");
                    for (int c=0;c<2;++c) for (int i=0;i<128;++i)
                        check(std::abs(routed.getSample(c,i)-0.25f*gain*std::sqrt(0.5f))<1.0e-6f,"other group keeps actual stereo route");
                    check(bankInvocations(100)==1,"both State groups reuse the same validated decoder result");
                    render(*window,"bank-state-other-group.png"); bankScopePriorState=current;
                    activateBankEvent(204); next(552);
                }
                break;
            case 552:
                if (!modal && document->getState() != bankScopePriorState && document->getFileInfo().numSamples == 144000)
                {
                    checkCompleteBankPreview(204, true);
                    check(bank->getAvailableStateValues().empty() && !bankStateControl(*window), "multi-target event exposes no false State control or remembered prior gain");
                    check(bank->getState()->events[1].stateTargetScope == BnkDocument::EventDescriptor::StateTargetScope::unavailable,
                        "actual native parser preserves explicit multi-target State boundary");
                    render(*window, "bank-state-multiple-targets.png");
                    std::cout << "Native State scope passed (actual ComboBox, complete target gain and routed PCM, multi-target reset and media preservation)\n";
                    bankScopePriorState.reset(); next(440);
                }
                break;
            case 520:
                if (modal) { check(clickModal("Cancel"), "cancel actual dirty-document close after completed bank work"); next(521); }
                break;
            case 521:
                if (!modal && elapsed > 100.0)
                {
                    const auto current = document->captureWorkspaceSnapshot();
                    check(current && bankWorkspace && current->sameContents(*bankWorkspace) && document->hasUnsavedChanges(),
                        "Cancel close preserves exact edited bank workspace and complete history");
                    const auto* list = findComponent<BnkAudioListViewComponent>(*window);
                    check(list && list->isShowing(), "Cancel close clears the settled bank loading state and restores its actual native list");
                    checkBankInputReleased(); checkVideoChildrenExited(); render(*window, "bank-cancel-close-workspace.png");
                    check(document->undo() && document->canRedo(), "actual edited bank history remains executable after Cancel close");
                    expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            case 440:
                if (!modal)
                {
                    checkVideoChildrenExited(); checkBankInputReleased(); render(*window, "bank-final-workspace.png");
                    expectedQuit = true; client.requestCloseApplication(); next(450);
                }
                break;
            default: break;
        }
    }
    static juce::Component* diagnosticChild(juce::Component& root, const juce::String& name)
    {
        if (root.getName() == name || root.getComponentID() == name) return &root;
        for (auto* child : root.getChildren()) if (auto* found = diagnosticChild(*child, name)) return found;
        return nullptr;
    }
    static juce::DocumentWindow* diagnosticsWindow()
    {
        for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
            if (auto* found = dynamic_cast<juce::DocumentWindow*>(juce::Desktop::getInstance().getComponent(i));
                found && found->getName() == "Local Diagnostics") return found;
        return nullptr;
    }
    void checkDiagnosticObservationLifetime()
    {
        using Operation = media::OptionalMediaOperation;
        using State = media::OptionalMediaFailureState;
        auto owner = std::make_shared<State>();
        const auto producer = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        auto operation = std::make_shared<Operation>(owner);
        check(operation->admitVideo(source, producer), "admit ordinary diagnostic lifetime fixture");
        const auto handle = operation->diagnosticHandle();
        const auto key = operation->report().key;
        std::weak_ptr<Operation> weakOperation = operation;
        operation.reset();
        check(weakOperation.expired() && handle->snapshot()->cancelled && !owner->inspect(*key).inFlight,
            "observation handle preserves destructor cancellation without retaining operation or in-flight policy");
        auto first = std::make_shared<Operation>(owner);
        check(first->admitVideo(source, producer), "cancelled attempt allows a new ordinary attempt");
        first->fail(State::Reason::decoderExit, State::Stage::decode);
        const auto firstReport = first->report();
        check(firstReport.failure && firstReport.failure->operationId == firstReport.operationId, "actual failure retains its request identity");
        auto retry = std::make_shared<Operation>(owner, State::Intent::retryOnce, firstReport.key);
        check(retry->admitVideo(source, producer), "reviewed retry remains usable with diagnostics enabled");
        retry->fail(State::Reason::decoderExit, State::Stage::decode);
        const auto retryReport = retry->report();
        check(retryReport.operationId != firstReport.operationId && retryReport.failure->operationId == retryReport.operationId,
            "retry has a fresh identity and owns its actual failure");
        auto denied = std::make_shared<Operation>(owner, State::Intent::retryOnce, retryReport.key);
        check(!denied->admitVideo(source, producer), "exhausted normal retry remains denied");
        const auto deniedHandle = denied->diagnosticHandle();
        const auto report = denied->report();
        check(report.failure && report.failure->operationId == retryReport.operationId && report.operationId != retryReport.operationId,
            "denied request refers to the original failure without relabelling it");
        const auto providers = diagnostics::captureLiveDiagnosticProviders(*diagnosticColdEngine, *diagnosticColdDocument, nullptr, deniedHandle);
        const auto codec = std::find_if(providers.begin(), providers.end(), [](const auto& provider) { return provider.kind == diagnostics::ProviderKind::codec; });
        check(codec != providers.end() && codec->evidence.latestFailure && codec->evidence.latestFailure->operationId == retryReport.operationId
            && codec->evidence.operationIds.size() == 2, "live provider carries original failure and current request identities separately");
        std::weak_ptr<State> weakOwner = owner;
        first.reset(); retry.reset(); denied.reset(); owner.reset();
        check(weakOwner.expired() && deniedHandle->snapshot()->terminal, "read-only result outlives neither session policy nor work owners");
        std::cout << "Diagnostic lifetime and original failure identity passed\n";
    }
    void startDiagnosticReview(MainComponent& client)
    {
        diagnosticSource = document->getFileInfo().file;
        if (!emptyCenter)
        {
            check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(), "prepare real editable history before passive diagnostics");
            diagnosticWorkspace = document->captureWorkspaceSnapshot();
        }
        diagnosticHadDevice = engine->getRuntimeMetricsForDiagnostics().has_value();
        if (deviceDiagnosticsMode)
        {
            engine->setMonitorGainLinear(0.0f); // isolated fixture: real transport without emitting test audio
            engine->setLoopEnabled(true);
            engine->play();
            check(engine->isPlaying(), "start real muted loop before native diagnostic workflow");
        }
        check(client.keyPressed(juce::KeyPress('p', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0)),
            "open real command palette through the component shortcut route; physical focus remains a user gate");
        next(700);
    }
    static juce::var reviewedProvider(const juce::String& preview, const juce::String& kind)
    {
        const auto section = preview.fromFirstOccurrenceOf("\nproviders/" + kind + ".json\n", false, false);
        return juce::JSON::parse(section.upToFirstOccurrenceOf("\nproviders/", false, false));
    }
    juce::var captureCurrentProvider(const char* section)
    {
        const auto captured = diagnostics::captureLiveDiagnosticProviders(*engine, *document, nullptr);
        std::vector<diagnostics::LoadedProviderRef> providers;
        for (const auto& provider : captured) providers.push_back({ provider.kind, &provider });
        const diagnostics::BundleIdentity identity { "0.3.0", "native-provider-fixture", juce::String::repeatedString("a", 64),
            "Release", "test", "x64", "x64", "test", false };
        const auto prepared = diagnostics::DiagnosticSnapshot::capture(identity, "test-fixture", providers);
        check(static_cast<bool>(prepared.status()), "current native provider snapshot is valid");
        for (const auto& entry : prepared.content().entries)
            if (entry.id == juce::String("providers/") + section + ".json") return juce::JSON::parse(entry.bytes.toString());
        throw std::runtime_error("missing current native provider");
    }
    bool chooseReviewedMediaDiagnosticFile(const juce::String& stem)
    {
        if (mediaDiagnosticChoosing)
        {
            if (!chooseOwnedFile(artifacts.getChildFile(stem + ".zip"))) return false;
            mediaDiagnosticChoosing = false;
            return true;
        }
        auto* dialog = diagnosticsWindow();
        auto* save = dialog ? button(*dialog, "Save reviewed bundle...") : nullptr;
        auto* preview = dialog ? dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview")) : nullptr;
        if (!save || !save->isEnabled() || !preview) return false;
        diagnosticReview = preview->getText();
        diagnosticWorkspace = document->captureWorkspaceSnapshot();
        check(artifacts.getChildFile(stem + "-review.txt").replaceWithText(diagnosticReview), "retain actual scoped preview evidence");
        render(*dialog, (stem + "-review.png").toRawUTF8());
        std::cout << "Media scope: modal=" << (juce::Component::getCurrentlyModalComponent() != nullptr)
            << " codec=" << diagnosticReview.contains("providers/codec.json") << " audio=" << diagnosticReview.contains("providers/audio.json")
            << " recovery=" << diagnosticReview.contains("providers/recovery.json") << " mediaChecked=" << button(*dialog, "Media")->getToggleState()
            << " audioChecked=" << button(*dialog, "Audio device")->getToggleState() << '\n';
        check(!juce::Component::getCurrentlyModalComponent() && diagnosticReview.contains("providers/codec.json")
            && !diagnosticReview.contains("providers/audio.json") && !diagnosticReview.contains("providers/recovery.json")
            && button(*dialog, "Media")->getToggleState() && !button(*dialog, "Audio device")->getToggleState(),
            "media failure enters the common nonmodal review with only related sections preselected");
        save->triggerClick(); mediaDiagnosticChoosing = true;
        return false;
    }
    bool finishReviewedMediaDiagnosticFile(const juce::String& stem)
    {
        auto* dialog = diagnosticsWindow();
        auto* save = dialog ? button(*dialog, "Save reviewed bundle...") : nullptr;
        if (!save || !save->isEnabled()) return false;
        const auto file = artifacts.getChildFile(stem + ".zip");
        check(static_cast<bool>(diagnostics::DiagnosticBundle::validate(file).status), "common media failure bundle validates");
        juce::ZipFile archive(file);
        const auto index = archive.getIndexOfFileName("providers/codec.json");
        check(index >= 0 && archive.getIndexOfFileName("providers/audio.json") < 0 && archive.getIndexOfFileName("providers/recovery.json") < 0,
            "preselected media scope reaches actual ZIP publication");
        std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(index));
        check(input != nullptr, "read actual common codec payload");
        const auto json = input->readEntireStreamAsString();
        const auto codec = juce::JSON::parse(json);
        check(diagnosticReview.contains(juce::JSON::toString(codec, false)) && !codec["latestFailure"].isVoid()
            && !juce::Uuid(codec["latestFailure"]["operationId"].toString()).isNull()
            && !json.contains(fixture.getFullPathName()) && (videoSource == juce::File() || !json.contains(videoSource.getFileName())) && !json.contains("native-preview.bnk")
            && document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace),
            "reviewed actual failure survives export with paths excluded and the complete original Document unchanged");
        diagnosticWorkspace.reset(); dialog->closeButtonPressed();
        return true;
    }
    void runDiagnosticsStep(MainComponent& client, double elapsed)
    {
        auto* dialog = diagnosticsWindow();
        auto* resourceCollector = dialog ? dynamic_cast<LocalDiagnosticsComponent*>(dialog->getContentComponent()) : nullptr;
        const auto remember = [](std::vector<std::weak_ptr<const void>>& observations, const std::weak_ptr<const void>& value)
        {
            if (value.expired()) return;
            if (std::none_of(observations.begin(), observations.end(), [&](const auto& prior)
                { return !value.owner_before(prior) && !prior.owner_before(value); })) observations.push_back(value);
        };
        if (resourceCollector)
        {
            const auto resources = resourceCollector->resourcesForTesting();
            remember(diagnosticJobs, resources.job);
            remember(diagnosticSnapshots, resources.snapshot);
            diagnosticSawChooser = diagnosticSawChooser || resources.choosingFile;
            check(diagnosticJobs.size() < 128 && diagnosticSnapshots.size() < 128, "bounded explicit diagnostic resource observations");
        }
        const auto text = [&]() -> juce::String
        {
            if (dialog) if (auto* editor = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"))) return editor->getText();
            return {};
        };
        const auto ready = [&]() { auto* save = dialog ? button(*dialog, "Save reviewed bundle...") : nullptr; return save && save->isEnabled(); };
        switch (stage)
        {
            case 700:
                check(!client.hasLocalDiagnosticsWindowForTesting() && client.deviceProbeForTesting().expired(),
                    "before explicit diagnostic command no native diagnostics window or device coordinator exists");
                if (auto* query = dynamic_cast<juce::TextEditor*>(diagnosticChild(client, "command-palette.search")))
                { query->setText("Local Diagnostics", true); next(701); }
                break;
            case 701:
                if (elapsed < 150.0) break;
                if (auto* run = button(client, "command-palette.run")) { run->triggerClick(); next(702); }
                break;
            case 702:
                if (!ready()) break;
                check(resourceCollector && !resourceCollector->resourcesForTesting().timerRunning
                    && resourceCollector->resourcesForTesting().job.expired() && !resourceCollector->resourcesForTesting().snapshot.expired(),
                    "ready review owns only its immutable snapshot, with no polling timer or collection job");
                check(!juce::Component::getCurrentlyModalComponent(), "diagnostics is a real nonmodal product window");
                diagnosticReview = text();
                if (deviceDiagnosticsMode) verifyActualNativeDeviceMetadata(*engine, reviewedProvider(diagnosticReview, "audio"), artifacts);
                check(diagnosticReview.contains("Snapshot:") && diagnosticReview.contains("-dirty")
                    && !diagnosticReview.contains(source.getFileName()) && (videoSource == juce::File() || !diagnosticReview.contains(videoSource.getFileName())) && !diagnosticReview.contains("selected local identifiers included"),
                    "default real snapshot excludes private source identity and honestly labels dirty build");
                if (!emptyCenter) check(diagnosticReview.contains("48000") && diagnosticReview.contains("providers/codec.json"), "real Document facts reach visible preview");
                else check(!diagnosticReview.contains("providers/codec.json"), "empty application does not invent a loaded codec provider");
                if (diagnosticOptionalScenario.isNotEmpty())
                {
                    const auto codec = reviewedProvider(diagnosticReview, "codec");
                    const auto cache = reviewedProvider(diagnosticReview, "cache");
                    const auto fields = codec["fields"], cacheFields = cache["fields"];
                    check(fields["lastOperationModule"]["value"].toString() == (packedMode ? "packed-audio" : "video-audio"), "real optional request module appears in the native review");
                    const auto ids = codec["operationIds"];
                    check(ids.size() == 1 && !juce::Uuid(ids[0].toString()).isNull(), "real operation identity appears in the native review");
                    if (diagnosticOptionalScenario.endsWith("failure"))
                    {
                        check(document->getFileInfo().file == source && document->canRedo(), "failed optional input preserves current ordinary audio and Redo");
                        check(fields["lastOperationState"]["value"].toString() == "failed"
                            && fields["lastFailureReason"]["value"].toString() == "decoder-exit", "actual child failure reaches the visible diagnostic fields");
                        check(codec["latestFailure"]["operationId"].toString() == ids[0].toString()
                            && static_cast<int>(codec["latestFailure"]["counters"][0]) == 37, "failure capsule identifies actual request and decoder exit 37");
                    }
                    else
                    {
                        check(fields["lastOperationState"]["value"].toString() == "succeeded"
                            && fields["lastOperationReader"]["value"].toString() == (packedMode ? "packed-progressive-wave" : "video-cache-reader"), "actual successful reader and completion are reported");
                        check(fields["selectedReader"]["value"].toString().startsWith("JUCE-WAV-")
                            && reviewedProvider(diagnosticReview, "spatial")["fields"]["channelOrder"]["value"].toString() == (packedMode ? "M" : "L R"),
                            "live decoded-wave reader and channel order remain distinct from the completed optional operation");
                        check(static_cast<int>(cacheFields["cacheSchema"]["value"]) == media::DecodedWaveCache::schemaVersion
                            && cache["operationIds"][0].toString() == ids[0].toString(), "actual cache schema and operation identity are correlated");
                        check(videoInvocations() == 1, "cache workflow starts exactly one actual child decoder");
                        if (!diagnosticCacheWarmed)
                        {
                            check(cacheFields["cacheResult"]["value"].toString() == "not-reused"
                                && cacheFields["cacheLookupReason"]["value"].toString() == (packedMode ? "entry-unavailable" : "seal-unavailable")
                                && cacheFields["cachePublication"]["value"].toString() == "published"
                                && static_cast<int>(cacheFields["cachePublications"]["value"]) == 1, "first request reports its actual complete cache publication");
                            diagnosticFirstOperation = ids[0].toString();
                            check(artifacts.getChildFile("first-cache-publication.txt").replaceWithText(diagnosticReview), "retain first actual cache observation");
                            render(*dialog, "diagnostics-first-cache-publication.png");
                            dialog->closeButtonPressed(); diagnosticCacheWarmed = true;
                            client.openInitialFile(source); next(723); break;
                        }
                        check(ids[0].toString() != diagnosticFirstOperation && cacheFields["cacheResult"]["value"].toString() == "reused"
                            && cacheFields["cacheLookupReason"]["value"].toString() == "reused"
                            && cacheFields["cachePublication"]["value"].toString() == "not-attempted"
                            && static_cast<int>(cacheFields["cacheHits"]["value"]) == 1
                            && static_cast<int>(cacheFields["cachePublications"]["value"]) == 0, "second request reuses the cache without a new publication or decoder process");
                    }
                }
                check(!diagnosticColdEngine->getRuntimeMetricsForDiagnostics()
                    && !diagnosticColdEngine->hasAmbisonicFieldAnalyzerForDiagnostics() && !diagnosticColdEngine->hasAmbisonicStageGraphForDiagnostics(),
                    "same real passive collector leaves an engine with no scheduled warmup entirely cold");
                std::cout << "Main device before/after snapshot: " << diagnosticHadDevice << "/"
                          << engine->getRuntimeMetricsForDiagnostics().has_value() << " (MainWindow independently schedules empty startup warmup at 900 ms)\n";
                render(*dialog, "diagnostics-default-preview.png");
                if (guiDeviceProbeMode)
                {
                    sourceHash = juce::SHA256(source).toHexString();
                    auto* manager = engine->getDeviceManagerForConfiguration();
                    check(manager != nullptr, "real idle manager exists before device probe");
                    const auto saved = manager->createStateXml();
                    probeDeviceState = saved ? saved->toString() : juce::String{};
                    probeDeviceSetup = manager->getAudioDeviceSetup(); probeBackend = manager->getCurrentAudioDeviceType();
                    button(*dialog, "Probe device...")->triggerClick(); next(800); break;
                }
                if (activeProbeMode)
                {
                    sourceHash = juce::SHA256(source).toHexString();
                    const auto savedDeviceState = engine->getDeviceManagerForConfiguration()->createStateXml();
                    probeDeviceState = savedDeviceState ? savedDeviceState->toString() : juce::String{};
                    probeDeviceSetup = engine->getDeviceManagerForConfiguration()->getAudioDeviceSetup();
                    probeBackend = engine->getDeviceManagerForConfiguration()->getCurrentAudioDeviceType();
                    check(source.copyFileTo(fixture.getChildFile("slow-probe.wav"))
                        && fixture.getChildFile("unreadable-probe.wav").replaceWithText("ordinary unsupported audio example"), "create actual probe sources");
                    button(*dialog, "Probe audio...")->triggerClick(); next(750); break;
                }
                if (emptyCenter)
                {
                    check(!button(*dialog, "Media")->isEnabled() && !button(*dialog, "Cache")->isEnabled()
                        && !button(*dialog, "Media")->getToggleState(), "absent modules cannot be selected or instantiated by the scope controls");
                    button(*dialog, "Save reviewed bundle...")->triggerClick(); next(703);
                }
                else { button(*dialog, "Media")->triggerClick(); next(712); }
                break;
            case 712:
                if (!ready() || text() == diagnosticReview) break;
                check(!text().contains("providers/codec.json") && !button(*dialog, "Media")->getToggleState(),
                    "visible user section toggle removes Media from a fresh immutable review");
                render(*dialog, "diagnostics-media-excluded.png");
                button(*dialog, "Media")->triggerClick(); next(713); break;
            case 713:
                if (!ready() || !text().contains("providers/codec.json")) break;
                diagnosticReview = text();
                check(button(*dialog, "Media")->getToggleState(), "user can restore an already-loaded section");
                button(*dialog, "Save reviewed bundle...")->triggerClick(); next(703); break;
            case 703:
                if (chooseOwnedFile(artifacts.getChildFile("reviewed-default.zip"))) next(704);
                break;
            case 704:
            {
                if (!ready()) break;
                check(resourceCollector && !resourceCollector->resourcesForTesting().timerRunning
                    && resourceCollector->resourcesForTesting().job.expired() && !resourceCollector->resourcesForTesting().snapshot.expired(),
                    "completed publication releases its worker job and timer while retaining the visible review");
                if (deviceDiagnosticsMode)
                {
                    const auto playing = engine->isPlaying();
                    const auto position = engine->getCurrentPositionSeconds();
                    std::cout << "Diagnostic export transport: playing=" << playing << " position=" << position
                        << " duration=" << engine->getDurationSeconds() << " looping=" << engine->isLoopEnabled()
                        << " device=" << engine->getRuntimeMetricsForDiagnostics().has_value() << "\n";
                    check(playing && position > 0.0, "real transport continues advancing during preview and native local export");
                }
                const auto file = artifacts.getChildFile("reviewed-default.zip");
                check(file.existsAsFile(), "explicit native Save creates the local bundle");
                const auto view = diagnostics::DiagnosticBundle::validate(file);
                check(static_cast<bool>(view.status), "product bundle validates through the actual archive reader");
                juce::ZipFile archive(file);
                for (int i = 0; i < archive.getNumEntries(); ++i)
                {
                    const auto* entry = archive.getEntry(i); auto input = archive.createStreamForEntry(i);
                    check(input != nullptr, "read published diagnostic payload");
                    const auto payload = input->readEntireStreamAsString();
                    check(!payload.contains(source.getFileName()) && !payload.contains(fixture.getFileName()), "default published payload excludes local identifiers");
                    if (entry->filename.startsWith("providers/"))
                        check(diagnosticReview.contains(juce::JSON::toString(juce::JSON::parse(payload), false)), "export contains exactly the reviewed provider bytes");
                    if (entry->filename == "manifest.json")
                    {
                        const auto manifest = juce::JSON::parse(payload);
                        check(diagnosticReview.contains(manifest["snapshotId"].toString())
                            && payload.contains(juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString()),
                            "review ID and actual running binary hash match the published manifest");
                    }
                }
                check(file.copyFileTo(artifacts.getChildFile("default-before-retry.zip")), "retain original bundle for no-overwrite verification");
                button(*dialog, "Save reviewed bundle...")->triggerClick(); next(705); break;
            }
            case 705:
                if (chooseOwnedFile(artifacts.getChildFile("reviewed-default.zip"))) next(706);
                break;
            case 706:
                if (!ready()) break;
                check(juce::SHA256(artifacts.getChildFile("reviewed-default.zip")) == juce::SHA256(artifacts.getChildFile("default-before-retry.zip")), "existing diagnostic bundle is never overwritten");
                if (auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status")))
                    check(status->getText().contains("already exists"), "existing destination refusal is visible");
                button(*dialog, "Include file name")->triggerClick(); next(707); break;
            case 707:
                if (!ready()) break;
                check(text() != diagnosticReview && text().contains("selected local identifiers included"), "privacy selection creates a new reviewed snapshot");
                if (!emptyCenter)
                    check(text().contains(diagnosticSource.getFileName()) && !text().contains(juce::JSON::toString(juce::var(diagnosticSource.getFullPathName()), true)), "filename opt-in does not opt into path");
                button(*dialog, "Include file path")->triggerClick(); next(708); break;
            case 708:
                if (!ready()) break;
                if (!emptyCenter) check(text().contains(juce::JSON::toString(juce::var(diagnosticSource.getFullPathName()), true)), "path appears only after its separate visible opt-in");
                render(*dialog, "diagnostics-selected-identifiers.png");
                if (reproductionDiagnosticsMode)
                {
                    diagnosticReview = text();
                    sourceHash = juce::SHA256(source).toHexString();
                    check(artifacts.getChildFile("private-reproduction.json").replaceWithText("{\"opaque-native-marker\":true}"), "create chosen opaque reproduction fixture");
                    button(*dialog, "Choose reproduction files...")->triggerClick(); next(730); break;
                }
                button(*dialog, "Refresh snapshot")->triggerClick(); next(711); break;
            case 730:
                if (chooseOwnedFile(artifacts.getChildFile("private-reproduction.json"))) next(731);
                break;
            case 731:
                if (clickModal("Cancel")) next(732);
                break;
            case 732:
                if (!ready()) break;
                check(text() == diagnosticReview && !button(*dialog, "Remove files")->isEnabled(),
                    "cancelling contents consent preserves the complete previously reviewed snapshot");
                button(*dialog, "Choose reproduction files...")->triggerClick(); next(733); break;
            case 733:
                if (chooseOwnedFile(artifacts.getChildFile("private-reproduction.json"), source)) next(734);
                break;
            case 734:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Include files"))
                { render(*modal, "reproduction-content-consent.png"); check(clickModal("Include files"), "explicitly consent to selected complete contents"); next(735); }
                break;
            case 735:
            {
                if (!ready()) break;
                check(text().contains("selected reproduction contents included without redaction") && text().contains("reproduction/file-1.bin")
                    && text().contains("Contents are not previewed.") && !text().contains("opaque-native-marker")
                    && !text().contains("private-reproduction.json"), "actual third-tier preview shows metadata only and never parses selected JSON");
                check(text().contains("reproduction/file-2.bin"), "actual native multiple selection captures both selected files");
                diagnosticReview = text(); render(*dialog, "reproduction-reviewed.png");
                if (auto* editor = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview")))
                { editor->moveCaretToEnd(); render(*dialog, "reproduction-metadata.png"); }
                const auto replacement = artifacts.getChildFile("replacement.json");
                check(replacement.replaceWithText("changed after native review") && replacement.replaceFileIn(artifacts.getChildFile("private-reproduction.json")),
                    "native review releases selected source before its atomic replacement");
                button(*dialog, "Save reviewed bundle...")->triggerClick(); next(736); break;
            }
            case 736:
                if (chooseOwnedFile(artifacts.getChildFile("reviewed-reproduction.zip")))
                {
                    auto* collector = dynamic_cast<LocalDiagnosticsComponent*>(dialog->getContentComponent());
                    check(collector != nullptr, "approved chooser still owns its real collector");
                    collector->requestScope(std::set<diagnostics::ProviderKind>{ diagnostics::ProviderKind::recovery });
                    next(737);
                }
                break;
            case 737:
            {
                if (!ready()) break;
                const auto output = artifacts.getChildFile("reviewed-reproduction.zip");
                const auto view = diagnostics::DiagnosticBundle::validate(output);
                check(static_cast<bool>(view.status) && diagnosticReview.contains(view.manifest["snapshotId"].toString()), "native attachment ZIP keeps the approved snapshot ID");
                juce::ZipFile zip(output);
                std::unique_ptr<juce::InputStream> bytes(zip.createStreamForEntry(zip.getIndexOfFileName("reproduction/file-1.bin")));
                check(bytes && bytes->readEntireStreamAsString() == "{\"opaque-native-marker\":true}"
                    && zip.getIndexOfFileName("providers/codec.json") >= 0 && text() != diagnosticReview && !text().contains("providers/codec.json")
                    && text().contains(juce::SHA256(artifacts.getChildFile("private-reproduction.json")).toHexString()),
                    "queued scope refresh waits for publication; approved ZIP keeps original sections and bytes while next review observes replacement");
                std::unique_ptr<juce::InputStream> audio(zip.createStreamForEntry(zip.getIndexOfFileName("reproduction/file-2.bin")));
                check(audio && juce::SHA256(*audio, -1).toHexString() == sourceHash && juce::SHA256(source).toHexString() == sourceHash,
                    "second selected source is copied exactly without disturbing live playback or changing its bytes");
                button(*dialog, "Remove files")->triggerClick(); next(738); break;
            }
            case 738:
                if (!ready() || text() == diagnosticReview) break;
                check(!text().contains("reproduction/file-") && text().contains("selected local identifiers included")
                    && !button(*dialog, "Remove files")->isEnabled(), "removing files returns to independently selected identifier consent");
                {
                    juce::FileOutputStream large(artifacts.getChildFile("large-reproduction.bin"));
                    check(large.openedOk() && large.setPosition(diagnostics::DiagnosticBundle::maximumEntryBytes) && large.writeByte(1), "write actual oversized selected file");
                }
                button(*dialog, "Choose reproduction files...")->triggerClick(); next(739); break;
            case 739:
                if (chooseOwnedFile(artifacts.getChildFile("large-reproduction.bin"))) next(740);
                break;
            case 740:
                if (clickModal("Include files")) next(741);
                break;
            case 741:
                if (auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status")); status && status->getText().contains("16 MiB"))
                {
                    check(!ready() && button(*dialog, "Remove files")->isEnabled(), "oversized input blocks publication but allows selection removal");
                    render(*dialog, "reproduction-budget-refusal.png");
                    button(*dialog, "Remove files")->triggerClick(); next(742);
                }
                break;
            case 742:
                if (!ready()) break;
                check(!text().contains("reproduction/file-"), "removing oversized selection restores a usable review");
                button(*dialog, "Choose reproduction files...")->triggerClick(); next(743); break;
            case 743:
                if (chooseOwnedFile(artifacts.getChildFile("private-reproduction.json"))) next(744);
                break;
            case 744:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Include files"))
                { dialog->closeButtonPressed(); next(709); }
                break;
            case 711:
                // triggerClick is asynchronous. Wait until its real handler has
                // disabled Save before closing, rather than deleting the button
                // before the queued refresh ever runs.
                if (ready()) break;
                check(dialog && dialog->isVisible(), "refresh is active in the real visible window before close");
                if (emptyCenter && !diagnosticReopenChecked)
                {
                    diagnosticReopenWindow = dialog;
                    dialog->closeButtonPressed();
                    client.showLocalDiagnosticsForTesting(); // same product entry before deferred close delivery
                    diagnosticReopenChecked = true; next(821); break;
                }
                dialog->closeButtonPressed(); next(709); break;
            case 821:
                if (!ready() || elapsed < 100.0) break;
                check(dialog && dialog->isVisible() && dialog == diagnosticReopenWindow.getComponent()
                    && client.hasLocalDiagnosticsWindowForTesting(),
                    "stale deferred close cannot delete a promptly reopened diagnostic window or its new capture");
                render(*dialog, "diagnostics-reopened-after-close.png");
                button(*dialog, "Save reviewed bundle...")->triggerClick(); next(822); break;
            case 822:
                if (!resourceCollector || !resourceCollector->resourcesForTesting().choosingFile || elapsed < 100.0) break;
                check(!resourceCollector->resourcesForTesting().snapshot.expired(), "native chooser callback holds the reviewed snapshot before cancellation");
                dialog->closeButtonPressed(); next(709); break;
            case 709:
                if (elapsed < 300.0) break;
                if (std::any_of(diagnosticJobs.begin(), diagnosticJobs.end(), [](const auto& value) { return !value.expired(); })
                    || std::any_of(diagnosticSnapshots.begin(), diagnosticSnapshots.end(), [](const auto& value) { return !value.expired(); }))
                    break; // Observe eventual worker cleanup before touching the continuing transport.
                if (activeProbeMode || guiDeviceProbeMode)
                {
                    if (!probeWorkerWitness.isEmpty() && !tests::decoderProcessExited(probeWorkerWitness)) break;
                    const auto savedDeviceState = engine->getDeviceManagerForConfiguration()->createStateXml();
                    check(juce::SHA256(source).toHexString() == sourceHash
                        && (savedDeviceState ? savedDeviceState->toString() : juce::String{}) == probeDeviceState
                        && engine->getDeviceManagerForConfiguration()->getAudioDeviceSetup() == probeDeviceSetup
                        && engine->getDeviceManagerForConfiguration()->getCurrentAudioDeviceType() == probeBackend,
                        "active probe success/failure/abort/window close leave source and exact device setup unchanged");
                }
                if (deviceDiagnosticsMode)
                {
                    const auto playing = engine->isPlaying();
                    const auto position = engine->getCurrentPositionSeconds();
                    std::cout << "Diagnostic final transport: playing=" << playing
                        << " position=" << position
                        << " duration=" << engine->getDurationSeconds()
                        << " looping=" << engine->isLoopEnabled()
                        << " device=" << engine->getRuntimeMetricsForDiagnostics().has_value() << "\n";
                    if (activeProbeMode || guiDeviceProbeMode)
                        check(!playing, "closing an active probe never restarts the deliberately stopped transport");
                    else check(playing && position > 0.0,
                        "real transport continues through privacy refresh and closing the pending collector");
                    engine->stop(); engine->setLoopEnabled(false);
                }
                check(dialog == nullptr && !client.hasLocalDiagnosticsWindowForTesting(),
                    "close releases the actual native diagnostic window, not just its contents/visibility");
                check(!diagnosticJobs.empty() && !diagnosticSnapshots.empty() && diagnosticSawChooser,
                    "release evidence covers actual jobs, reviewed snapshots and file chooser callbacks");
                {
                    juce::DynamicObject::Ptr released = new juce::DynamicObject;
                    released->setProperty("schema", "singleeditor.diagnostic-resource-release");
                    released->setProperty("version", 1);
                    released->setProperty("observedJobs", static_cast<int>(diagnosticJobs.size()));
                    released->setProperty("observedSnapshots", static_cast<int>(diagnosticSnapshots.size()));
                    released->setProperty("observedFileChooser", diagnosticSawChooser);
                    released->setProperty("allExpired", true);
                    released->setProperty("nativeWindowReleased", true);
                    released->setProperty("deviceCoordinatorReleased", client.deviceProbeForTesting().expired());
                    released->setProperty("afterCloseMs", elapsed);
                    check(artifacts.getChildFile("diagnostic-resource-release.json").replaceWithText(juce::JSON::toString(juce::var(released.get()))),
                        "retain actual post-export and post-close resource release evidence");
                }
                if (!emptyCenter)
                {
                    const auto current = document->captureWorkspaceSnapshot();
                    check(current && diagnosticWorkspace && current->sameContents(*diagnosticWorkspace) && document->canRedo(), "diagnostics preserves real Document and full Redo history");
                    check(document->redo() && document->undo(), "retained Undo and Redo remain executable after diagnostics closes");
                }
                check(!diagnosticColdEngine->getRuntimeMetricsForDiagnostics(), "collector did not schedule later device activation on the cold engine");
                diagnosticColdEngine.reset(); diagnosticColdDocument.reset();
                diagnosticReleasedActivity = diagnostics::testing::activitySnapshot();
                next(823); break;
            case 823:
                if (elapsed < 500.0) break;
                check(diagnostics::testing::activitySnapshot() == diagnosticReleasedActivity,
                    "after release no late collector, worker, provider, file access or probe entry resumes");
                {
                    juce::DynamicObject::Ptr activity = new juce::DynamicObject;
                    activity->setProperty("schema", "singleeditor.diagnostic-idle-activity");
                    activity->setProperty("version", 1);
                    activity->setProperty("preTriggerAllZero", diagnosticStartupQuiet);
                    activity->setProperty("positiveProviderControl", true);
                    activity->setProperty("postReleaseUnchangedMs", elapsed);
                    juce::Array<juce::var> counts;
                    for (const auto count : diagnosticReleasedActivity) counts.add(static_cast<juce::int64>(count));
                    activity->setProperty("entryOrder", "component job worker providers snapshot identity nativeMetadata reproduction publication validation codecProbe deviceProbe");
                    activity->setProperty("observedEntries", counts);
                    check(artifacts.getChildFile("diagnostic-idle-activity.json").replaceWithText(juce::JSON::toString(juce::var(activity.get()))),
                        "retain pre-trigger and post-release native entry observations");
                }
                expectedQuit = true; client.requestCloseApplication(); next(710); break;
            case 710:
                if (clickModal("Discard")) return;
                if (clickModal("Exit Anyway")) return;
                break;
            case 800:
                if (auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status")); status && status->getText().contains("open, idle"))
                {
                    check(engine->isPlaying() && ready() && text() == diagnosticReview, "GUI device probe refuses busy playback without interruption");
                    engine->stop(); button(*dialog, "Probe device...")->triggerClick(); next(801);
                }
                break;
            case 801:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Review effects"))
                { render(*modal, "device-probe-options.png"); check(clickModal("Cancel"), "cancel device options before any effect"); next(802); }
                break;
            case 802:
                if (!ready()) break;
                check(text() == diagnosticReview && !engine->isDeviceLeasedForProbe(), "declining options keeps exact device and previous snapshot");
                button(*dialog, "Probe device...")->triggerClick(); next(803); break;
            case 803:
                if (clickModal("Review effects")) next(804);
                break;
            case 804:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Run device probe"))
                { render(*modal, "device-probe-effect-consent.png"); check(clickModal("Cancel"), "decline separate device effect consent"); next(805); }
                break;
            case 805:
                if (!ready()) break;
                check(text() == diagnosticReview && !engine->isDeviceLeasedForProbe(), "second consent decline also starts no device work");
                button(*dialog, "Probe device...")->triggerClick(); next(806); break;
            case 806:
                if (clickModal("Review effects")) next(807);
                break;
            case 807:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Run device probe"))
                { engine->play(); check(clickModal("Run device probe"), "confirm device probe after playback starts"); next(808); }
                break;
            case 808:
                if (auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status")); status && status->getText().contains("Device probe refused"))
                {
                    check(engine->isPlaying() && !engine->isDeviceLeasedForProbe() && text() == diagnosticReview,
                        "device effect completion rechecks actual activity before transferring any manager");
                    engine->stop(); engine->seekToSeconds(0.375); probeRetainedPosition = engine->getCurrentPositionSeconds();
                    button(*dialog, "Probe device...")->triggerClick(); next(809);
                }
                break;
            case 809:
                if (clickModal("Review effects")) next(810);
                break;
            case 810:
                if (clickModal("Run device probe")) next(811);
                break;
            case 811:
            {
                if (!ready() || text() == diagnosticReview) break;
                const auto audio = reviewedProvider(text(), "audio");
                const auto fields = audio["fields"];
                check(fields["negotiationRequestKind"]["value"].toString() == "gui-active-probe"
                    && fields["negotiationOutcome"]["value"].toString() == "succeeded"
                    && fields["deviceProbeRestore"]["value"].toString() == "exact-device-restored"
                    && fields["requestedChannelPolicy"]["value"].toString() == "exact-physical-mask-" + probeDeviceSetup.outputChannels.toString(16)
                    && fields["deviceProbeAction"]["value"].toString() == "silent-negotiation"
                    && !engine->isDeviceLeasedForProbe() && !engine->isPlaying(),
                    "actual GUI transfers open device, runs real product silent worker, and restores exact setup without transport restart");
                check(!text().contains("providers/codec.json") && !text().contains(probeDeviceSetup.outputDeviceName),
                    "GUI device observation is separate and excludes the private output name by default");
                diagnosticReview = text(); probeOperation = audio["operationIds"][0].toString();
                render(*dialog, "device-probe-restored-report.png");
                check(artifacts.getChildFile("device-probe-restored-review.txt").replaceWithText(diagnosticReview), "retain actual GUI device observation");
                button(*dialog, "Save reviewed bundle...")->triggerClick(); next(812); break;
            }
            case 812:
                if (chooseOwnedFile(artifacts.getChildFile("reviewed-device-probe.zip"))) next(813);
                break;
            case 813:
            {
                if (!ready()) break;
                const auto output = artifacts.getChildFile("reviewed-device-probe.zip");
                const auto validated = diagnostics::DiagnosticBundle::validate(output);
                check(validated.status && validated.verifiedEntries == 1 && validated.manifest["origin"].toString() == "gui",
                    "actual device observation exports through reviewed GUI snapshot schema");
                juce::ZipFile archive(output);
                std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(archive.getIndexOfFileName("providers/audio.json")));
                check(input && juce::JSON::parse(input->readEntireStreamAsString())["operationIds"][0].toString() == probeOperation,
                    "published device report preserves the exact operation identity");
                button(*dialog, "Refresh snapshot")->triggerClick(); next(814); break;
            }
            case 814:
                if (!ready() || text() == diagnosticReview) break;
                check(reviewedProvider(text(), "audio")["operationIds"][0].toString() == probeOperation,
                    "refresh retains the same actual device operation rather than probing again");
                button(*dialog, "Current state")->triggerClick(); next(815); break;
            case 815:
                if (!ready() || !text().contains("providers/codec.json")) break;
                check(reviewedProvider(text(), "audio")["fields"]["negotiationRequestKind"]["value"].toString() != "gui-active-probe",
                    "Current state explicitly returns to actual restored GUI owners");
                button(*dialog, "Probe device...")->triggerClick(); next(816); break;
            case 816:
                if (clickModal("Review effects")) next(817);
                break;
            case 817:
                if (clickModal("Run device probe")) next(818);
                break;
            case 818:
                if (!probeAdmissionLogged)
                {
                    std::cout << "Device cancel admission: leased=" << engine->isDeviceLeasedForProbe()
                        << " modal=" << (juce::Component::getCurrentlyModalComponent() != nullptr)
                        << " elapsed=" << elapsed << "\n";
                    probeAdmissionLogged = true;
                }
                // triggerClick() queues a JUCE click/consent callback. Observe
                // actual transfer before asserting an in-flight cancellation.
                if (!engine->isDeviceLeasedForProbe() && elapsed < 2000.0) break;
                check(engine->isDeviceLeasedForProbe() && button(*dialog, "Abort probe")->isEnabled(),
                    "second actual device probe is in flight before abort and window close");
                std::cout << "Device cancel actual lease observed after " << elapsed << " ms\n";
                probeLifetime = client.deviceProbeForTesting();
                check(!probeLifetime.expired(), "actual live device coordinator is observable without retaining it");
                button(*dialog, "Abort probe")->onClick();
                dialog->closeButtonPressed(); next(819); break;
            case 819:
                if (engine->isDeviceLeasedForProbe()) break;
                if (!probeLifetime.expired() && elapsed < 2000.0) break;
                check(probeLifetime.expired() && client.deviceProbeForTesting().expired(),
                    "after exact device return and window close no Main or UI owner retains the diagnostic coordinator");
                check(engine->getRuntimeMetricsForDiagnostics().has_value(), "Main completes restoration after diagnostic window is destroyed");
                check(std::abs(engine->getCurrentPositionSeconds() - probeRetainedPosition) < 1.0 / 48000.0,
                    "actual callback removal/reprepare preserves the stopped playhead through probe and cancellation");
                next(709); break;
            case 750:
                if (auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status")); status && status->getText().contains("Stop playback"))
                {
                    check(ready() && text() == diagnosticReview && engine->isPlaying(), "active playback refuses the probe without stopping transport or replacing the snapshot");
                    engine->stop(); button(*dialog, "Probe audio...")->triggerClick(); next(751);
                }
                break;
            case 751:
                if (chooseOwnedFile(source)) next(752);
                break;
            case 752:
                if (clickModal("Cancel")) next(753);
                break;
            case 753:
                if (!ready()) break;
                check(text() == diagnosticReview, "declining active-probe consent keeps the old reviewed state");
                button(*dialog, "Probe audio...")->triggerClick(); next(754); break;
            case 754:
                if (chooseOwnedFile(source)) next(755);
                break;
            case 755:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Run probe"))
                { engine->play(); check(clickModal("Run probe"), "confirm while transport became active"); next(756); }
                break;
            case 756:
                if (auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status")); status && status->getText().contains("Probe refused"))
                {
                    check(ready() && text() == diagnosticReview && engine->isPlaying(), "consent completion rechecks activity and cannot stop playback");
                    engine->stop(); button(*dialog, "Probe audio...")->triggerClick(); next(758);
                }
                break;
            case 758:
                if (chooseOwnedFile(probePhase == 1 ? fixture.getChildFile("unreadable-probe.wav") : source)) next(759);
                break;
            case 759:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && button(*modal, "Run probe"))
                { if (probePhase == 0) render(*modal, "active-probe-effect-consent.png"); check(clickModal("Run probe"), "explicitly confirm bounded codec read"); next(760); }
                break;
            case 760:
            {
                if (!ready() || text() == diagnosticReview) break;
                const auto codec = reviewedProvider(text(), "codec");
                check(codec["fields"]["lastOperationModule"]["value"].toString() == "gui-codec-probe"
                    && text().contains("not the current Document") && !text().contains("providers/audio.json")
                    && !text().contains(source.getFileName()), "actual probe report is distinct from current loaded state and default source identifiers remain excluded");
                diagnosticReview = text();
                if (probePhase == 1)
                {
                    check(codec["fields"]["lastOperationState"]["value"].toString() == "failed" && codec.hasProperty("latestFailure"), "unsupported real audio reports a true decode failure");
                    probeFailure = codec["latestFailure"]["operationId"].toString();
                    probePhase = 2; button(*dialog, "Probe audio...")->triggerClick(); next(758); break;
                }
                check(codec["fields"]["selectedReader"]["value"].toString() == "JUCE-WAV"
                    && static_cast<int>(codec["fields"]["lastOperationReadyFrames"]["value"]) == 4096, "real companion observes a bounded actual WAV window");
                if (probePhase == 2)
                {
                    check(codec["latestFailure"]["operationId"].toString() == probeFailure
                        && codec["operationIds"][0].toString() != probeFailure, "successful later probe retains the previous failure with its distinct operation identity");
                    render(*dialog, "active-probe-recovered.png");
                    if (realProbeCompanion) { dialog->closeButtonPressed(); next(709); break; }
                    button(*dialog, "Probe audio...")->triggerClick(); next(769); break;
                }
                probeOperation = codec["operationIds"][0].toString();
                render(*dialog, "active-probe-report.png");
                button(*dialog, "Save reviewed bundle...")->triggerClick(); next(761); break;
            }
            case 761:
                if (chooseOwnedFile(artifacts.getChildFile("reviewed-probe.zip"))) next(762);
                break;
            case 762:
            {
                if (!ready()) break;
                const auto output = artifacts.getChildFile("reviewed-probe.zip");
                const auto view = diagnostics::DiagnosticBundle::validate(output);
                check(view.status && view.verifiedEntries == 1 && view.manifest["origin"].toString() == "gui"
                    && diagnosticReview.contains(view.manifest["snapshotId"].toString()), "active result exports through the shared reviewed GUI bundle path");
                juce::ZipFile zip(output); std::unique_ptr<juce::InputStream> input(zip.createStreamForEntry(zip.getIndexOfFileName("providers/codec.json")));
                check(input && juce::JSON::parse(input->readEntireStreamAsString())["operationIds"][0].toString() == probeOperation,
                    "actual published report retains the exact probe identity");
                button(*dialog, "Refresh snapshot")->triggerClick(); next(763); break;
            }
            case 763:
                if (!ready() || text() == diagnosticReview) break;
                check(reviewedProvider(text(), "codec")["operationIds"][0].toString() == probeOperation,
                    "snapshot refresh does not rerun the active operation");
                button(*dialog, "Current state")->triggerClick(); next(764); break;
            case 764:
                if (!ready() || !text().contains("providers/audio.json")) break;
                check(!text().contains("gui-codec-probe") && !button(*dialog, "Current state")->isEnabled(), "explicit Current state returns to loaded owners without a probe rerun");
                diagnosticReview = text(); probePhase = 1;
                button(*dialog, "Probe audio...")->triggerClick(); next(758); break;
            case 769:
                if (chooseOwnedFile(fixture.getChildFile("slow-probe.wav"))) next(770);
                break;
            case 770:
                if (clickModal("Run probe")) next(771);
                break;
            case 771:
                if (const auto marker = fixture.getChildFile("probe-worker-live.txt"); marker.existsAsFile())
                {
                    probeWorkerWitness = marker.loadFileAsString();
                    check(!probeWorkerWitness.isEmpty() && !tests::decoderProcessExited(probeWorkerWitness), "actual owned reader child is live before cancellation");
                    if (probeCancelMode == 3) { dialog->closeButtonPressed(); next(709); break; }
                    if (probeCancelMode == 2) { next(773); break; }
                    if (probeCancelMode == 0) { render(*dialog, "active-probe-abort.png"); button(*dialog, "Abort probe")->triggerClick(); }
                    else engine->play();
                    next(772);
                }
                break;
            case 772:
                if (!ready()) break;
                check(elapsed < 2000.0 && tests::decoderProcessExited(probeWorkerWitness) && text() == diagnosticReview,
                    "explicit abort or resumed playback promptly ends the real child and preserves the previous immutable review");
                if (probeCancelMode == 1) { check(engine->isPlaying(), "probe abort does not undo the user's new playback"); engine->stop(); }
                check(fixture.getChildFile("probe-worker-live.txt").deleteFile(), "release only the owned prior worker witness before next attempt");
                ++probeCancelMode; button(*dialog, "Probe audio...")->triggerClick(); next(769); break;
            case 773:
            {
                if (!ready() || text() == diagnosticReview) break;
                const auto codec = reviewedProvider(text(), "codec");
                check(elapsed < 7000.0 && tests::decoderProcessExited(probeWorkerWitness)
                    && static_cast<int>(codec["fields"]["codecFailureDetail"]["value"]) == static_cast<int>(media::BoundedDecodeProcess::Failure::deadline)
                    && static_cast<int>(codec["latestFailure"]["counters"][4]) == 1,
                    "real five-second worker limit publishes a typed deadline after its process lease is fully closed");
                diagnosticReview = text(); render(*dialog, "active-probe-deadline.png");
                check(fixture.getChildFile("probe-worker-live.txt").deleteFile(), "remove the completed deadline witness before owner-close scenario");
                ++probeCancelMode; button(*dialog, "Probe audio...")->triggerClick(); next(769); break;
            }
            case 720:
                if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable")
                {
                    check(videoInvocations() == 1, "one real child failure precedes native diagnostic review");
                    check(clickModal("Keep current audio"), "retain ordinary audio after optional decoder failure"); next(722);
                }
                break;
            case 721:
                if (document->hasAudio() && document->getFileInfo().file == videoSource && waveform->isPeakCacheReady())
                {
                    if (packedMode && !media::DecodedWaveCache::sealFile(VgmstreamDecodeService::getDecodedWaveFile(videoSource)).existsAsFile()) break;
                    startDiagnosticReview(client);
                }
                break;
            case 722:
                if (!juce::Component::getCurrentlyModalComponent())
                {
                    const auto current = document->captureWorkspaceSnapshot();
                    check(current && diagnosticWorkspace && current->sameContents(*diagnosticWorkspace) && document->canRedo(),
                        "actual failed optional open preserves the entire pre-existing ordinary workspace and Redo");
                    startDiagnosticReview(client);
                }
                break;
            case 723:
                if (document->getFileInfo().file == source && waveform->isPeakCacheReady())
                { client.openInitialFile(videoSource); next(721); }
                break;
            default: break;
        }
    }
    void runHrtfStep(double elapsed)
    {
        auto* client = window ? window->getMainComponent() : nullptr;
        if (!client) return;
        switch (stage)
        {
            case 0:
                if (!client->isCoreReadyForWorkspaceResources()) break;
                if (hrtfScenario == "hrtf-normal" && !hrtfStartupRendererPreparationObserved) break;
                if (hrtfScenario == "hrtf-normal")
                    check(hrtfStartupRendererPreparedAfterCore,
                        "startup renderer preparation follows actual CoreReady, not full-cache arrival");
                check(ambisonicResources::packReadAttempts == 0 && !document->getState()->ambisonicBinauralFilterBank,
                    "ordinary four-channel CoreReady has not read the HRTF pack");
                check(document->setChannelLayoutPreset("foa-acn-sn3d"), "interpret the real file as FOA");
                next(1); break;
            case 1:
            {
                auto* mode = combo(*window, "Ambisonic monitor mode");
                if (!mode || !mode->isEnabled()) break;
                if (!mode->isShowing())
                {
                    auto* disclosure = button(*window, "Channel monitor...");
                    check(disclosure != nullptr, "actual metadata disclosure can reveal the HRTF control");
                    disclosure->triggerClick(); break;
                }
                check(ambisonicResources::packReadAttempts == 0, "FOA speaker controls do not read the pack");
                mode->setSelectedId(2, juce::sendNotificationSync);
                check(!document->getState()->ambisonicBinauralFilterBank && mode->getText() == "Preparing HRTF...",
                    "actual ComboBox requests background preparation without synchronous publication");
                if (hrtfScenario == "hrtf-stale" || hrtfScenario == "hrtf-close")
                {
                    for (int i = 0; i < 200 && ambisonicResources::packReadAttempts == 0; ++i) juce::Thread::sleep(1);
                    check(ambisonicResources::packReadAttempts == 1 && ambisonicResources::workerReads == 1,
                        "one real worker starts while its UI completion is deliberately pending");
                    if (hrtfScenario == "hrtf-close")
                    {
                        window.reset(); stopTimer(); expectedQuit = true; quit(); return;
                    }
                    check(document->setAmbisonicOrientation({45.0f, 0.0f, 0.0f}), "replace exact monitor state before late result");
                    hrtfState = document->getState();
                    next(4); break;
                }
                next(2); break;
            }
            case 2:
                if (hrtfScenario == "hrtf-missing" || hrtfScenario == "hrtf-corrupt")
                {
                    auto* modal = juce::Component::getCurrentlyModalComponent();
                    if (!modal || modal->getName() != "Binaural HRTF unavailable") break;
                    check(document->getAmbisonicMonitorMode() == AmbisonicMonitorMode::speakerProjection
                        && !document->getState()->ambisonicBinauralFilterBank && document->getAmbisonicBinauralStatus().isNotEmpty()
                        && ambisonicResources::preparedBanks == 0, "unavailable pack keeps speaker monitoring and an explicit reason");
                    render(*modal, "hrtf-unavailable.png");
                    check(clickModal("OK"), "dismiss HRTF resource message"); next(5); break;
                }
                if (!document->getState()->ambisonicBinauralFilterBank) break;
                check(document->getAmbisonicMonitorMode() == AmbisonicMonitorMode::binauralHrtf
                    && ambisonicResources::packReadAttempts == 1 && ambisonicResources::workerReads == 1
                    && ambisonicResources::preparedBanks == 1, "one background pack read prepares the actual HRTF bank");
                hrtfLifetime = document->getState()->ambisonicBinauralFilterBank;
                {
                    juce::AudioBuffer<float> rendered(2, 256);
                    check(engine->renderDocumentBlockForDiagnostics(0, rendered)
                        && engine->isAmbisonicBinauralRendererActive(),
                        "prepare and render the actual HRTF owner before testing release");
                }
                render(*window, "hrtf-active.png");
                combo(*window, "Ambisonic monitor mode")->setSelectedId(1, juce::sendNotificationSync);
                next(3); break;
            case 3:
                if (elapsed < 250.0) break;
                check(!document->getState()->ambisonicBinauralFilterBank && hrtfLifetime.expired(),
                    "returning to speaker releases the document and rendered engine filter-bank leases");
                next(5); break;
            case 4:
                if (elapsed < 250.0) break;
                check(document->getState() == hrtfState && !document->getState()->ambisonicBinauralFilterBank
                    && document->getAmbisonicOrientation().yawDegrees == 45.0f,
                    "a cancelled late worker cannot overwrite changed state despite equal changeGeneration");
                next(5); break;
            case 5:
                if (juce::Component::getCurrentlyModalComponent()) break;
                check(document->hasAudio() && !document->hasUnsavedChanges() && waveform->isPeakCacheReady(),
                    "ordinary audio and full waveform remain usable after the optional resource flow");
                check(artifacts.getChildFile("hrtf-result.txt").replaceWithText(hrtfScenario + "\nreads="
                    + juce::String(ambisonicResources::packReadAttempts.load()) + "\nworker_reads="
                    + juce::String(ambisonicResources::workerReads.load()) + "\nprepared="
                    + juce::String(ambisonicResources::preparedBanks.load())), "write actual HRTF lifecycle counts");
                stopTimer(); expectedQuit = true; quit(); break;
            default: break;
        }
    }

    void runTrayStep(double elapsed)
    {
        auto* client = window->getMainComponent();
        switch (stage)
        {
            case 0:
                if (trayScenario == "tray-empty")
                {
                    if (!client || elapsed < 300.0) break;
                    check(windowShellResources::packReadAttempts == 0 && windowShellResources::imageDecodes == 0
                        && window->hasNativeTrayIconForTesting() && !window->isShowing(),
                        "empty workspace retains fallback without reading a pack header");
                    window->restoreFromBackground();
                    check(window->isShowing() && window->trayResourceStatusForTesting() == "absent", "empty tray restores and releases");
                    client->openInitialFile(source); next(1); break;
                }
                if (!client || !client->isCoreReadyForWorkspaceResources()) break;
                if (trayScenario != "tray-precore")
                {
                    check(windowShellResources::packReadAttempts == 0 && windowShellResources::imageDecodes == 0,
                        "actual CoreReady alone does not read the optional tray pack");
                    window->requestUserCloseAction();
                }
                next(2); break;
            case 1:
                if (client && client->isCoreReadyForWorkspaceResources())
                { window->requestUserCloseAction(); next(2); }
                break;
            case 2:
            {
                const auto status = window->trayResourceStatusForTesting();
                if (trayScenario == "tray-cancel" && status == "loading")
                {
                    window->restoreFromBackground(); next(3); break;
                }
                if (status == "waiting-for-core" || status == "loading") break;
                check(trayScenario != "tray-cancel", "release exercised while the worker result was still pending UI publication");
                const auto expected = trayScenario == "tray-missing" ? "tray_pack_unavailable"
                    : trayScenario == "tray-corrupt" ? "tray_pack_identity_mismatch" : "ready";
                check(status == expected && window->hasNativeTrayIconForTesting() && !window->isShowing(),
                    "actual hidden window retains native tray with the expected normal or fallback result");
                check(windowShellResources::packReadAttempts == 1
                    && windowShellResources::imageDecodes == (status == "ready" ? 1 : 0),
                    "one triggered bounded read and decoding only for the valid pack");
                window->restoreFromBackground(); next(3); break;
            }
            case 3:
                if (elapsed < 250.0) break;
                check(window->isShowing() && !window->hasNativeTrayIconForTesting()
                    && window->trayResourceStatusForTesting() == "absent",
                    "restore removes the native tray and no late worker recreates it");
                check(document->hasAudio() && !document->hasUnsavedChanges() && waveform->isPeakCacheReady()
                    && document->getFileInfo().file == source,
                    "normal document and waveform survive tray resource success, failure or release");
                render(*window, "restored-window.png");
                check(artifacts.getChildFile("tray-result.txt").replaceWithText(trayScenario + "\nreads="
                    + juce::String(windowShellResources::packReadAttempts.load()) + "\ndecodes="
                    + juce::String(windowShellResources::imageDecodes.load()) + "\ntray-released=true\nordinary-document-ready=true\n"),
                    "retain actual tray lifecycle result");
                expectedQuit = true; window->requestActualQuit(); next(4); break;
            default: break;
        }
    }

    void timerCallback() override
    {
        try
        {
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - stageStarted;
            lifecycle_test::validateScenarioDeadline(elapsed, capacityMode ? 600000.0 : 45000.0);
            if (trayScenario.isNotEmpty()) { runTrayStep(elapsed); return; }
            if (hrtfScenario.isNotEmpty()) { runHrtfStep(elapsed); return; }
            auto* client = window ? window->getMainComponent() : nullptr;
            if (!client) return;
            if (realBankInput != juce::File()) { runRealBankStep(*client, elapsed); return; }
            std::optional<ScopedOptionalLifecycleContext> backendContext;
            if (backendLifecycle)
            {
                backendContext.emplace(backendLifecycleContext(*backendLifecycle, *client));
                sampleBackendLifecycleTests(*backendLifecycle, *client);
            }
            if (magLifecycleMode)
            {
                if (!magLifecycleCompleted && advanceMagLifecycleTests(*magLifecycle, *client, *engine))
                {
                    magLifecycleCompleted = true;
                    expectedQuit = true;
                    window->requestActualQuit();
                }
                return;
            }
            if (optionalLifecycleMode)
            {
                if (!optionalLifecycleCompleted && advanceOptionalLifecycleTests(*optionalLifecycle, *client, *engine))
                {
                    optionalLifecycleCompleted = true;
                    expectedQuit = true;
                    window->requestActualQuit();
                }
                return;
            }
            if (diagnosticsMode && stage >= 700) { runDiagnosticsStep(*client, elapsed); return; }
            if (bankScenario.isNotEmpty() && stage >= 400) { runBankNativeStep(*client, elapsed); return; }
            switch (stage)
            {
                case 0:
                    if (packedStartupMode)
                    {
                        if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "WEM audio unavailable")
                        {
                            check(videoInvocations() == 1 && !document->hasAudio() && window->isShowing(), "startup WEM failure reaches one session owner without fallback decode or invented audio");
                            render(*modal, "wem-startup-failure.png"); check(clickModal("Keep current audio"), "dismiss startup WEM failure"); next(330);
                        }
                        break;
                    }
                    if (videoStartupMode)
                    {
                        if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable")
                        {
                            check(videoInvocations() == 1 && !document->hasAudio() && window->isShowing(), "startup optional failure is attempted once without inventing usable audio");
                            render(*modal, "video-startup-failure.png"); check(clickModal("Keep current audio"), "dismiss startup failure"); next(230);
                        }
                        break;
                    }
                    if (emptyCenter || (document->hasAudio() && waveform->isPeakCacheReady())) next(1);
                    break;
                case 1:
                    if (elapsed < 500.0) break;
                    if (diagnosticsMode)
                    {
                        const auto beforeTrigger = diagnostics::testing::activitySnapshot();
                        check(std::all_of(beforeTrigger.begin(), beforeTrigger.end(), [](auto count) { return count == 0; }),
                            "actual ordinary startup creates no diagnostic UI/job/worker or provider/IO/probe entry before an explicit trigger");
                        diagnosticStartupQuiet = true;
                        diagnosticColdDocument = std::make_unique<SingleTrackDocument>();
                        diagnosticColdEngine = std::make_unique<AudioEngine>(*diagnosticColdDocument);
                        check(!diagnosticColdEngine->getRuntimeMetricsForDiagnostics(), "real isolated engine starts without a device or warmup request");
                        for (int capture = 0; capture < 3; ++capture)
                        {
                            const auto facts = diagnostics::captureLiveDiagnosticProviders(*diagnosticColdEngine, *diagnosticColdDocument, nullptr);
                            check(facts.size() == 1 && facts.front().kind == diagnostics::ProviderKind::audio
                                && std::any_of(facts.front().evidence.fields.begin(), facts.front().evidence.fields.end(), [](const auto& field)
                                    { return field.field == diagnostics::Field::deviceState && field.value.toString() == "uninitialised"; })
                                && std::all_of(facts.front().evidence.fields.begin(), facts.front().evidence.fields.end(), [](const auto& field)
                                    { return field.field == diagnostics::Field::deviceState || field.value.isVoid(); })
                                && !diagnosticColdEngine->getRuntimeMetricsForDiagnostics(),
                                "repeated real cold-owner capture reports unavailable audio without constructing devices, codecs or recovery");
                        }
                        auto expectedActivity = beforeTrigger;
                        expectedActivity[static_cast<std::size_t>(diagnostics::testing::Activity::providers)] += 3;
                        check(diagnostics::testing::activitySnapshot() == expectedActivity,
                            "three deliberate provider captures activate the idle detector without constructing diagnostics or entering IO");
                        checkDiagnosticObservationLifetime();
                        if (spatialDiagnosticsMode) runSpatialEnergyNativeTests(artifacts);
                        // The dedicated diagnostics-device invocation owns the
                        // complete device/Spatial suite. Codec/reproduction
                        // invocations exercise their own real end-to-end flows.
                        else if (deviceDiagnosticsMode && !activeProbeMode && !reproductionDiagnosticsMode)
                        {
                            if (backendLifecycle) deviceBackendLifecycle = runDeviceBackendLifecycleTests(artifacts);
                            else runAudioDeviceDiagnosticsTests(properties, artifacts);
                        }
                        if (diagnosticOptionalScenario.isNotEmpty())
                        {
                            if (diagnosticOptionalScenario.endsWith("failure"))
                            {
                                check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(), "retain real history before failed optional request");
                                diagnosticWorkspace = document->captureWorkspaceSnapshot();
                            }
                            client->openInitialFile(videoSource); next(diagnosticOptionalScenario.endsWith("failure") ? 720 : 721);
                        }
                        else startDiagnosticReview(*client);
                        break;
                    }
                    if (bankScenario != "bank-fallback-recovered" && (bankScenario.startsWith("bank-open-") || bankScenario.startsWith("bank-fallback-"))) { startBankOpenScenario(*client); break; }
                    if (bankScenario.isNotEmpty() && bankScenario != "bank-fallback-recovered") { createNativeBank(); client->openInitialFile(packedBankFile); next(400); break; }
                    if (packedBankMode)
                    {
                        juce::MemoryBlock pcm; check(source.loadFileAsData(pcm), "read owned PCM bank payload");
                        juce::MemoryOutputStream out;
                        out.write("BKHD", 4); out.writeInt(8); out.writeInt(128); out.writeInt(7);
                        out.write("DIDX", 4); out.writeInt(12); out.writeInt(99); out.writeInt(0); out.writeInt(static_cast<int>(pcm.getSize()));
                        out.write("DATA", 4); out.writeInt(static_cast<int>(pcm.getSize())); out.write(pcm.getData(), pcm.getSize());
                        packedBankFile = fixture.getChildFile("return.bnk");
                        check(packedBankFile.replaceWithData(out.getData(), out.getDataSize()), "create actual minimal embedded PCM bank");
                        client->openInitialFile(packedBankFile); next(350); break;
                    }
                    if (packedMode && !packedRecoveredMode)
                    {
                        check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(), "real ordinary return image retains Redo");
                        videoOriginalState = document->getState();
                        check(engine->renderDocumentBlockForDiagnostics(0, videoOriginalPcm), "capture actual original PCM");
                        retainPackedSelection();
                        client->openInitialFile(videoSource); next(300); break;
                    }
                    if (videoMode)
                    {
                        check(document->moveEnvelopePoint(0, 0, 0.43f) && document->undo() && document->canRedo(), "real clean current image has retained Redo");
                        videoOriginalState = document->getState();
                        check(engine->renderDocumentBlockForDiagnostics(0, videoOriginalPcm), "render actual current PCM before optional failure");
                        client->openInitialFile(videoSource); next(videoBudgetMode ? 220 : 200); break;
                    }
                    if (centerMode)
                    {
                        if (cleanupScenario.startsWith("cleanup-auto-"))
                        {
                            const auto* service = client->recoveryServiceForTesting();
                            if (!service || service->health().needsPoll || !service->health().maintenance.sweep.completed) break;
                            check(service->health().completedOwners == 1 && service->health().maintenance.failure
                                && !centerWindow() && !juce::Component::getCurrentlyModalComponent()
                                && !document->hasUnsavedChanges() && !engine->isPlaying()
                                && !files::nativeFile(journal.getChildFile(firstOwner.toString())).exists()
                                && !files::nativeFile(journal.getChildFile("terminal-" + firstOwner.toString())).exists()
                                && juce::SHA256(source).toHexString() == sourceHash,
                                "real PostCore automatically completes old terminal cleanup without changing or selecting current audio");
                            check(document->moveEnvelopePoint(0, 0, 0.57f) && document->moveEnvelopePoint(0, 0, 0.64f)
                                && document->undo() && document->canRedo(), "native editing and complete history remain usable after automatic cleanup");
                            centerImage = document->captureWorkspaceSnapshot(); next(1104); break;
                        }
                        if (!statusVisible()) break;
                        check(!centerWindow() && !juce::Component::getCurrentlyModalComponent() && !document->hasUnsavedChanges()
                            && !engine->isPlaying(), "discovery is passive and does not activate a recovery document");
                        if (emptyCenter)
                        {
                            check(!document->hasAudio() && window->isShowing(), "empty workspace is interactive without invented audio readiness");
                            render(*window, "recovery-center-empty-workspace.png");
                        }
                        clickStatus(); next(40); break;
                    }
                    if (invalidRoot)
                    {
                        check(statusVisible() && !recoveryRoot.exists() && !fixture.getChildFile("journals").exists(),
                            "invalid native root is visible without a relative storage fallback");
                        check(document->moveEnvelopePoint(0, 0, 0.43f), "actual edit with unavailable recovery root");
                        diagnosticWorkspace = document->captureWorkspaceSnapshot();
                        clickStatus(); next(920); break;
                    }
                    check(!recoveryRoot.exists() && !statusVisible() && !engine->isPlaying(), "real clean native open remains recovery-inert and does not auto-play");
                    check(document->moveEnvelopePoint(0, 0, 0.43f) && document->hasUnsavedChanges() && document->canUndo(), "actual committed edit remains immediately visible with Undo");
                    if (capacityMode)
                    {
                        check(document->moveEnvelopePoint(0, 0, 0.64f) && document->undo(), "large native editor keeps both history branches during protection");
                        diagnosticWorkspace = document->captureWorkspaceSnapshot(); next(940); break;
                    }
                    next(2); break;
                case 940:
                    if (statusVisible() || !protectedOwner(firstOwner)) break;
                    {
                        const auto manifest = recovery::RecoveryStore(journal, firstOwner).inspectManifest();
                        if (!manifest.status || manifest.head.sequence != 3) break;
                        recovery::PreparedRecoveryCheckpoint prepared;
                        check(static_cast<bool>(recovery::RecoveryCheckpointCodec::prepareThrough(manifest, manifest.head, prepared))
                            && prepared.getUndoCount() == 1 && prepared.getRedoCount() == 1,
                            "native long source publishes the exact current Undo and Redo branches");
                        check(document->canUndo() && document->canRedo() && !engine->isPlaying()
                            && document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace),
                            "actual background large protection preserves current history and transport");
                        render(*window, "capacity-protected.png");
                        window->getMainComponent()->showRecoveryCenterForTesting(); next(944);
                    }
                    break;
                case 944:
                    if (auto* center = centerWindow())
                    {
                        check(!juce::Component::getCurrentlyModalComponent() && button(*center, "Storage"),
                            "protected large source opens the real nonmodal management entry");
                        render(*center, "capacity-recovery-center.png");
                        check(document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace),
                            "opening capacity management preserves the current document and full history");
                        window.reset(); stopTimer(); expectedQuit = true; quit(); return;
                    }
                    break;
                case 2:
                    if (!protectedOwner(firstOwner) || statusVisible()) break;
                    render(*window, "protected-general-client-render.png"); saveThroughNativeCommand(); next(3); break;
                case 3:
                    if (document->hasUnsavedChanges() || statusVisible()
                        || files::nativeFile(journal.getChildFile(firstOwner.toString())).exists()) break;
                    check(document->canUndo() && !engine->isPlaying(), "native Save adopts clean state, preserves Undo and retires its exact owner");
                    holdQuota(); check(document->moveEnvelopePoint(0, 0, 0.62f), "create actual edit while the private storage lock is held"); next(4); break;
                case 4:
                    if (!statusVisible() || elapsed < (sessionDiagnosticsMode ? 1000.0 : 250.0)) break;
                    if (sessionDiagnosticsMode)
                        check(document->moveEnvelopePoint(0, 0, 0.51f), "real edit after blocked packet age triggers ordinary Session protection gap");
                    checkStatusLayout();
                    render(*window, "degraded-general-client-render.png"); clickStatus(); next(5); break;
                case 5:
                    if (clickModal("Diagnostics")) next(6);
                    break;
                case 6:
                    if (diagnosticsWindow() && !juce::Component::getCurrentlyModalComponent())
                    { diagnosticWorkspace = document->captureWorkspaceSnapshot(); next(763); }
                    break;
                case 763:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "actual recovery diagnostic preview exists");
                            diagnosticReview = preview->getText();
                            check(!diagnosticReview.contains("providers/audio.json") && !diagnosticReview.contains("providers/codec.json")
                                && button(*dialog, "Recovery")->getToggleState() && !button(*dialog, "Media")->getToggleState(),
                                "Recovery failure button opens the shared review with Recovery preselected");
                            const auto recovery = reviewedProvider(diagnosticReview, "recovery");
                            const auto fields = recovery["fields"], failure = recovery["latestFailure"];
                            const auto phase = fields["recoveryFailureStage"]["value"].toString();
                            check(fields["recoveryStage"]["value"].toString() == "degraded"
                                && fields["recoveryFailureCoverage"]["value"].toString() == "writer-maintenance-inspection-restore-session-host-service"
                                && (sessionDiagnosticsMode ? phase == "session-transition" : phase == "writer-prepare" || phase == "writer-append")
                                && fields["recoveryFailureOwner"]["value"].toString().length() == 64
                                && fields["checkpointGeneration"]["unavailable"].toString() == "not-reported"
                                && !juce::Uuid(failure["operationId"].toString()).isNull()
                                && recovery["operationIds"][0].toString() == failure["operationId"].toString()
                                && static_cast<int>(failure["code"]) == static_cast<int>(sessionDiagnosticsMode ? recovery::Error::budgetExceeded : recovery::Error::resourceOrEnvironmentFault),
                                "actual writer or Session gap supplies a typed failure and operation ID without inventing checkpoint generation");
                            if (sessionDiagnosticsMode)
                            {
                                sessionDiagnosticFailure = juce::Uuid(failure["operationId"].toString());
                                sessionDiagnosticPhase = phase;
                                check(static_cast<int64_t>(failure["counters"][0]) > 0,
                                    "Session failure includes its actual identity event sequence");
                            }
                            check(!diagnosticReview.contains(source.getFileName()) && !diagnosticReview.contains(fixture.getFullPathName()),
                                "default recovery diagnostics omit local names and paths");
                            render(*dialog, "recovery-unified-preview.png");
                            save->triggerClick(); next(764);
                        }
                    break;
                case 764:
                    if (chooseOwnedFile(artifacts.getChildFile("recovery-reviewed.zip"))) next(765);
                    break;
                case 765:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            const auto file = artifacts.getChildFile("recovery-reviewed.zip");
                            check(static_cast<bool>(diagnostics::DiagnosticBundle::validate(file).status), "actual recovery ZIP validates");
                            juce::ZipFile archive(file);
                            const auto index = archive.getIndexOfFileName("providers/recovery.json");
                            check(index >= 0, "actual recovery provider was exported");
                            std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(index));
                            check(input != nullptr && diagnosticReview.contains(juce::JSON::toString(juce::JSON::parse(input->readEntireStreamAsString()), false)),
                                "published recovery failure is exactly the reviewed immutable payload");
                            check(document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace) && quotaBlock,
                                "review and export neither retry the blocked writer nor alter the real Document history");
                            diagnosticWorkspace.reset(); dialog->closeButtonPressed(); next(7);
                        }
                    break;
                case 7:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    client->requestCloseApplication(); next(8); break;
                case 8:
                    if (clickModal("Discard")) next(9);
                    break;
                case 9:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery still pending")
                    {
                        check(quotaBlock && !expectedQuit && document->hasUnsavedChanges(), "real held quota keeps the native close unfinished");
                        pendingCloseWorkspace = document->captureWorkspaceSnapshot();
                        check(pendingCloseWorkspace && engine->renderDocumentBlockForDiagnostics(0, pendingClosePcm),
                            "capture actual full history and PCM before explicit close Wait");
                        render(*modal, "close-pending-wait-before.png");
                        check(clickModal("Wait"), "explicit Wait does not retry or discard failed recovery work"); next(180);
                    }
                    break;
                case 180:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); elapsed >= 800.0
                        && modal && modal->getName() == "Recovery still pending")
                    {
                        const auto current = document->captureWorkspaceSnapshot();
                        check(quotaBlock && current && current->sameContents(*pendingCloseWorkspace) && window->isVisible(),
                            "persistent real recovery refusal after Wait retains the exact Document and full Undo/Redo");
                        juce::AudioBuffer<float> pcm(2, 128);
                        check(engine->renderDocumentBlockForDiagnostics(0, pcm), "render after actual bounded Wait");
                        for (int channel = 0; channel < 2; ++channel) for (int sample = 0; sample < 128; ++sample)
                            check(pcm.getSample(channel, sample) == pendingClosePcm.getSample(channel, sample),
                                "explicit Wait retains actual PCM while recovery is unfinished");
                        render(*modal, "close-pending-client-render.png");
                        std::cout << "Native Recovery Wait passed (real held quota, second prompt, exact workspace and PCM, no forced exit)\n";
                        pendingCloseWorkspace.reset(); check(clickModal("Cancel"), "cancel actual bounded-close prompt"); next(10);
                    }
                    break;
                case 10:
                    if (juce::Component::getCurrentlyModalComponent() || elapsed < 250.0) break;
                    check(document->hasUnsavedChanges() && document->canUndo() && window->isVisible(), "Cancel preserves native Document and full history");
                    if (sessionDiagnosticsMode) { clickStatus(); next(913); break; }
                    quotaBlock.reset(); clickStatus(); next(11); break;
                case 11:
                    if (clickModal("Retry")) next(12);
                    break;
                case 12:
                    if (juce::Component::getCurrentlyModalComponent() || statusVisible()) break;
                    { juce::Uuid resumed; if (!protectedOwner(resumed)) break; }
                    if (sessionDiagnosticsMode)
                    {
                        diagnosticWorkspace = document->captureWorkspaceSnapshot();
                        check(client->keyPressed(juce::KeyPress('p', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0)),
                            "open real command palette after successful Session repair");
                        next(910); break;
                    }
                    saveThroughNativeCommand(); next(13); break;
                case 910:
                    if (auto* query = dynamic_cast<juce::TextEditor*>(diagnosticChild(*client, "command-palette.search")))
                    { query->setText("Local Diagnostics", true); next(911); }
                    break;
                case 911:
                    if (elapsed < 150.0) break;
                    if (auto* run = button(*client, "command-palette.run")) { run->triggerClick(); next(912); }
                    break;
                case 912:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "actual repaired Session diagnostic preview exists");
                            const auto result = reviewedProvider(preview->getText(), "recovery");
                            check(juce::Uuid(result["latestFailure"]["operationId"].toString()) == sessionDiagnosticFailure
                                && result["fields"]["recoveryFailureStage"]["value"].toString() == sessionDiagnosticPhase
                                && result["fields"]["recoveryProtectionReason"]["value"].toString() == "none",
                                "successful real repair clears current protection reason while retaining the last actual pre-retry failure");
                            check(document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace)
                                && document->undo() && document->redo()
                                && document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace),
                                "repair and diagnostic refresh preserve actual full workspace and executable Undo/Redo");
                            render(*dialog, "session-repaired-diagnostics.png");
                            diagnosticWorkspace.reset(); dialog->closeButtonPressed();
                            saveThroughNativeCommand(); next(13);
                        }
                    break;
                case 913:
                    if (clickModal("Diagnostics")) next(914);
                    break;
                case 914:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "cancelled-close readmission has a real diagnostic preview");
                            const auto result = reviewedProvider(preview->getText(), "recovery");
                            const juce::Uuid operation(result["latestFailure"]["operationId"].toString());
                            const auto phase = result["fields"]["recoveryFailureStage"]["value"].toString();
                            check(!operation.isNull() && operation != sessionDiagnosticFailure
                                && (phase == "writer-prepare" || phase == "writer-append"),
                                "real new checkpoint failure while storage is still held correctly replaces the earlier Session gap");
                            sessionDiagnosticFailure = operation; sessionDiagnosticPhase = phase;
                            render(*dialog, "session-cancelled-close-diagnostics.png");
                            dialog->closeButtonPressed(); quotaBlock.reset(); clickStatus(); next(11);
                        }
                    break;
                case 13:
                    if (document->hasUnsavedChanges() || statusVisible()) break;
                    if (auto* simple = button(*client, "workspace.view.simple"); simple && simple->isEnabled())
                    { simple->triggerClick(); next(14); }
                    break;
                case 14:
                    if (elapsed < 250.0) break;
                    holdQuota(); check(document->moveEnvelopePoint(0, 0, 0.31f), "create actual Simple-view protection failure"); next(15); break;
                case 15:
                    if (!statusVisible() || elapsed < 250.0) break;
                    checkStatusLayout();
                    render(*window, "degraded-simple-client-render.png"); client->requestCloseApplication(); next(16); break;
                case 16:
                    if (clickModal("Discard")) next(17);
                    break;
                case 17:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery still pending")
                    { expectedQuit = true; check(clickModal("Exit Anyway"), "explicit native Exit Anyway"); next(18); }
                    break;
                case 40:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery")
                    { check(clickModal("Recovery Center"), "open Center from existing protection actions"); break; }
                    if (cleanupScenario.isNotEmpty())
                    {
                        const auto* service = client->recoveryServiceForTesting();
                        if (!service || service->health().needsPoll || service->health().maintenance.failure) break;
                        if (auto* center = centerWindow())
                            if (auto* records = combo(*center, "Recovery records"); records && records->isEnabled() && records->getNumItems() > 0)
                            {
                                for (int i = 0; i < records->getNumItems(); ++i)
                                {
                                    records->setSelectedItemIndex(i, juce::sendNotificationSync);
                                    if (centerText().contains(firstOwner.toString())) break;
                                }
                                auto* resume = button(*center, "Resume cleanup");
                                check(centerText().contains(firstOwner.toString()) && resume && resume->isEnabled()
                                    && !button(*center, "Review Discard")->isEnabled() && !button(*center, "Restore Copy")->isEnabled(),
                                    "persisted terminal record offers Resume without offering another Discard or Restore");
                                quotaBlock.reset();
                                check(document->moveEnvelopePoint(0, 0, 0.57f) && document->moveEnvelopePoint(0, 0, 0.64f)
                                    && document->undo() && document->canRedo(), "current document has actual Undo and Redo before cleanup");
                                centerImage = document->captureWorkspaceSnapshot();
                                render(*center, "recovery-persisted-cleanup-summary.png");
                                next(1099);
                            }
                        break;
                    }
                    if (storageMode && !storageChecked)
                    {
                        if (auto* center = centerWindow())
                            if (auto* storage = button(*center, "Storage"); storage && storage->isEnabled())
                            {
                                check(document->moveEnvelopePoint(0, 0, 0.47f) && document->moveEnvelopePoint(0, 0, 0.64f)
                                    && document->undo() && document->canRedo(), "prepare real complete current history before storage inspection");
                                centerImage = document->captureWorkspaceSnapshot(); next(1118);
                            }
                        break;
                    }
                    if (auto* center = centerWindow())
                        if (auto* action = button(*center, "Review Discard"); action && action->isEnabled())
                        {
                            check(!juce::Component::getCurrentlyModalComponent(), "Recovery Center itself is nonmodal");
                            check(centerText().contains(firstOwner.toString()), "Center selects the actual previous owner");
                            render(*center, "recovery-center-summary.png");
                            if (emptyCenter && !document->hasAudio())
                            { client->openInitialFile(source); next(60); break; }
                            check(document->moveEnvelopePoint(0, 0, 0.57f), "current audio remains editable while Center is open");
                            if (restoreMode)
                            {
                                centerImage = document->captureWorkspaceSnapshot();
                                if (inspectionMode)
                                { button(*center, "Safe Inspect")->triggerClick(); next(80); break; }
                                check(recoveryRoot.getSiblingFile("RecoveryScratch").replaceWithText("owned scratch obstruction"), "create actual private scratch failure");
                                auto* restore = button(*center, "Restore Copy"); check(restore && restore->isEnabled(), "actual Restore Copy is available");
                                restore->triggerClick(); next(70); break;
                            }
                            centerImage = document->captureWorkspaceSnapshot();
                            if (storageMode) { button(*center, "Review expiry...")->triggerClick(); next(1210); }
                            else { action->triggerClick(); next(41); }
                        }
                    break;
                case 1210:
                case 1211:
                    if (auto* modal = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
                        modal && modal->getName() == "Review recovery expiry")
                    {
                        check(modal->getTextEditorContents("days").isEmpty(), "expiry has no implicit destructive retention default");
                        modal->getTextEditor("days")->setText("1");
                        check(clickModal("Review"), "user explicitly chooses one-day age for native expiry review");
                        next(stage == 1210 ? 41 : 44);
                    }
                    break;
                case 41:
                    if (auto* center = centerWindow())
                        if (auto* action = button(*center, "Discard..."); action && action->isEnabled())
                        {
                            check(centerText().contains("Observed stored bytes:") && centerText().contains("Last safe time:")
                                && centerText().contains(firstOwner.toString()), "actual owner, size and time precede confirmation");
                            if (storageMode) check(centerText().contains("Expiry review: keep at least 1 days."), "native review displays chosen expiry age");
                            render(*center, "recovery-center-review.png"); action->triggerClick(); next(42);
                        }
                    break;
                case 42:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Discard recovery record?")
                    { render(*modal, "recovery-center-confirmation.png"); check(clickModal("Cancel"), "Cancel second Discard confirmation"); next(43); }
                    break;
                case 43:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    check(files::nativeFile(journal.getChildFile(firstOwner.toString())).exists()
                        && document->captureWorkspaceSnapshot()->sameContents(*centerImage), "Cancel preserves old record and current complete history");
                    if (auto* center = centerWindow())
                    {
                        auto* later = button(*center, "Later");
                        check(later && later->isEnabled(), "actual Recovery Center exposes Later");
                        deferredCenter = center;
                        render(*center, "recovery-center-later.png");
                        later->triggerClick(); next(1050);
                    }
                    break;
                case 1050:
                    if (elapsed < 250.0) break;
                    check(!deferredCenter && !client->hasRecoveryCenterForTesting() && !centerWindow(),
                        "Later releases the actual Center view and native window after its callback returns");
                    check(recovery::RecoveryStore(journal, firstOwner).inspectManifest().status
                        && juce::SHA256(source).toHexString() == sourceHash
                        && document->captureWorkspaceSnapshot()->sameContents(*centerImage),
                        "Later preserves the durable owner, source and complete active workspace");
                    check(document->undo() && document->redo() && document->captureWorkspaceSnapshot()->sameContents(*centerImage),
                        "Later preserves executable active Undo and Redo");
                    check(client->keyPressed(juce::KeyPress('p', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0)),
                        "open actual command palette after deferring recovery");
                    next(1051); break;
                case 1051:
                    if (auto* query = dynamic_cast<juce::TextEditor*>(diagnosticChild(*client, "command-palette.search")))
                    { query->setText("Recovery Center", true); next(1052); }
                    break;
                case 1052:
                    if (elapsed < 150.0) break;
                    if (auto* run = button(*client, "command-palette.run")) { run->triggerClick(); next(1053); }
                    break;
                case 1053:
                    if (auto* center = centerWindow())
                        if (auto* records = combo(*center, "Recovery records"); records && records->isEnabled() && records->getNumItems() > 0)
                        {
                            for (int i = 0; i < records->getNumItems(); ++i)
                            {
                                records->setSelectedItemIndex(i, juce::sendNotificationSync);
                                if (centerText().contains(firstOwner.toString())) break;
                            }
                            check(centerText().contains(firstOwner.toString()), "explicit command rediscovers the deferred durable owner");
                            deferredCenter = center;
                            center->closeButtonPressed();
                            client->showRecoveryCenterForTesting();
                            next(1054);
                        }
                    break;
                case 1054:
                    if (elapsed < 250.0) break;
                    check(deferredCenter && centerWindow() == deferredCenter.getComponent() && centerWindow()->isVisible(),
                        "queued close disposal cannot delete a Center explicitly reopened before delivery");
                    if (auto* action = button(*centerWindow(), "Review Discard"))
                    {
                        if (storageMode) { button(*centerWindow(), "Review expiry...")->triggerClick(); next(1211); }
                        else { action->triggerClick(); next(44); }
                    }
                    break;
                case 44:
                    if (auto* center = centerWindow())
                        if (auto* action = button(*center, "Discard..."); action && action->isEnabled())
                        { holdQuota(); action->triggerClick(); next(45); }
                    break;
                case 45:
                    if (clickModal("Discard recovery record")) next(46);
                    break;
                case 46:
                    if (juce::Component::getCurrentlyModalComponent() || elapsed < 300.0 || !centerText().contains("Discard accepted")) break;
                    check(files::nativeFile(journal.getChildFile(firstOwner.toString())).exists(), "real root contention preserves accepted cleanup for Retry");
                    if (auto* center = centerWindow()) center->closeButtonPressed();
                    quotaBlock.reset(); clickStatus(); next(47); break;
                case 47:
                    if (clickModal("Retry")) next(48);
                    break;
                case 48:
                    if (juce::Component::getCurrentlyModalComponent() || files::nativeFile(journal.getChildFile(firstOwner.toString())).exists()) break;
                    check(!centerWindow() && !client->hasRecoveryCenterForTesting(), "closed Center releases its view without cancelling background Discard");
                    check(document->captureWorkspaceSnapshot()->sameContents(*centerImage) && juce::SHA256(source).toHexString() == sourceHash
                        && !engine->isPlaying(), "real native Discard leaves source and active full history unchanged");
                    check(!owners().empty(), "current independently protected owner is preserved");
                    render(*window, "recovery-center-after-discard.png"); expectedQuit = true; client->requestCloseApplication(); next(49); break;
                case 49:
                    if (clickModal("Discard")) next(50);
                    break;
                case 1099:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        if (health.needsPoll || !std::any_of(health.protection.sessions.begin(), health.protection.sessions.end(),
                            [](const auto& session) { return session.active && session.protection.currentProtected; })) break;
                        holdQuota(); button(*centerWindow(), "Resume cleanup")->triggerClick(); next(1100);
                    }
                    break;
                case 1100:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        if (!centerText().contains("Cleanup resume requested") || health.maintenance.workerRunning || health.maintenance.failure) break;
                        check(!health.maintenance.jobs.empty() && health.maintenance.jobs.front().owner == firstOwner
                            && files::nativeFile(journal.getChildFile(firstOwner.toString())).exists(),
                            "real contention keeps the resumed terminal job and its remaining data owned");
                        render(*centerWindow(), "recovery-persisted-cleanup-pending.png");
                        deferredCenter = centerWindow(); centerWindow()->closeButtonPressed();
                        quotaBlock.reset(); clickStatus(); next(1101);
                    }
                    break;
                case 1101:
                    if (clickModal("Retry")) next(1102);
                    break;
                case 1102:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        if (health.needsPoll || health.completedOwners < 1 || juce::Component::getCurrentlyModalComponent()) break;
                        check(!deferredCenter && !client->hasRecoveryCenterForTesting()
                            && !files::nativeFile(journal.getChildFile(firstOwner.toString())).exists()
                            && !files::nativeFile(journal.getChildFile("terminal-" + firstOwner.toString())).exists(),
                            "background Retry completes the old terminal cleanup after actual View destruction");
                        check(document->captureWorkspaceSnapshot()->sameContents(*centerImage) && document->undo() && document->redo()
                            && document->captureWorkspaceSnapshot()->sameContents(*centerImage)
                            && juce::SHA256(source).toHexString() == sourceHash && !engine->isPlaying(),
                            "resumed cleanup preserves current workspace, executable full history and the original audio");
                        client->showRecoveryCenterForTesting(); next(1103);
                    }
                    break;
                case 1118:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        if (health.needsPoll || !std::any_of(health.protection.sessions.begin(), health.protection.sessions.end(),
                            [](const auto& session) { return session.active && session.protection.currentProtected; })) break;
                        button(*centerWindow(), "Storage")->triggerClick(); next(1110);
                    }
                    break;
                case 1110:
                    if (auto* center = centerWindow())
                    {
                        const auto text = centerText("Recovery storage details");
                        if (!text.contains("Observed use:") || !text.contains("Stored protection limits")) break;
                        int64_t bytes = 0;
                        for (const auto& file : files::nativeFile(recoveryRoot).findChildFiles(juce::File::findFiles, true)) bytes += file.getSize();
                        const auto* service = client->recoveryServiceForTesting();
                        check(service != nullptr, "actual service supplies current storage configuration");
                        const auto ownerLimit = service->configuredOwnerByteLimit();
                        std::cout << "Storage observations: visible=" << diagnosticChild(*center, "Recovery storage details")->isShowing()
                            << " recordsHidden=" << !diagnosticChild(*center, "Recovery record details")->isShowing()
                            << " countedBytes=" << bytes << " bytesMatch=" << text.contains("(" + juce::String(bytes) + " bytes)")
                            << " totalLimit=" << text.contains("8589934592 bytes")
                            << " ownerLimit=" << ownerLimit << " ownerMatch="
                            << text.contains("Current per-document recovery budget: " + juce::File::descriptionOfSizeInBytes(ownerLimit)
                                             + " (" + juce::String(ownerLimit) + " bytes)")
                            << " disk=" << text.contains("Available on disk:")
                            << " modal=" << (juce::Component::getCurrentlyModalComponent() != nullptr)
                            << " history=" << document->captureWorkspaceSnapshot()->sameContents(*centerImage)
                            << " source=" << (juce::SHA256(source).toHexString() == sourceHash) << std::endl;
                        check(diagnosticChild(*center, "Recovery storage details")->isShowing()
                            && !diagnosticChild(*center, "Recovery record details")->isShowing()
                            && text.contains("(" + juce::String(bytes) + " bytes)") && text.contains("8589934592 bytes")
                            && text.contains("Current per-document recovery budget: " + juce::File::descriptionOfSizeInBytes(ownerLimit)
                                             + " (" + juce::String(ownerLimit) + " bytes)") && text.contains("Available on disk:")
                            && !juce::Component::getCurrentlyModalComponent()
                            && document->captureWorkspaceSnapshot()->sameContents(*centerImage) && juce::SHA256(source).toHexString() == sourceHash,
                            "actual native Storage view shows measured bytes and real limits without changing current audio/history");
                        render(*center, "recovery-storage-usage.png");
                        if (retentionMode)
                        {
                            check(text.contains("Automatic cleanup: not configured") && button(*center, "Retention...")->isEnabled(),
                                "unconfigured retention has no silent destructive default");
                            button(*center, "Retention...")->triggerClick(); next(1300); break;
                        }
                        storageReadEntered = std::make_shared<juce::WaitableEvent>();
                        storageReadRelease = std::make_shared<juce::WaitableEvent>();
                        auto* content = dynamic_cast<RecoveryCenterComponent*>(center->getContentComponent()); check(content != nullptr, "actual Recovery Center content");
                        content->setStorageReadBoundaryForTesting([entered = storageReadEntered, release = storageReadRelease]
                        { entered->signal(); (void) release->wait(5000); });
                        button(*center, "Refresh usage")->triggerClick(); next(1111);
                    }
                    break;
                case 1300:
                case 1302:
                case 1307:
                    if (auto* modal = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
                        modal && modal->getName() == "Automatic recovery retention")
                    {
                        check(modal->getTextEditorContents("days") == (stage == 1307 ? "1" : ""),
                            "policy editor uses only the actual configured period");
                        modal->getTextEditor("days")->setText(stage == 1307 ? "30" : "1");
                        check(clickModal("Review policy"), "review explicit native retention period");
                        next(stage == 1300 ? 1301 : stage == 1302 ? 1303 : 1308);
                    }
                    break;
                case 1301:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent();
                        modal && modal->getName() == "Apply automatic recovery cleanup?")
                    {
                        render(*modal, "recovery-retention-confirmation.png");
                        check(clickModal("Cancel"), "cancel automatic policy before writing"); next(1310);
                    }
                    break;
                case 1310:
                    if (!juce::Component::getCurrentlyModalComponent())
                    {
                        check(!recoveryRoot.getChildFile("retention-policy").exists() && journal.getChildFile(firstOwner.toString()).exists(),
                            "Cancel preserves unconfigured policy and old recovery copy");
                        button(*centerWindow(), "Retention...")->triggerClick(); next(1302);
                    }
                    break;
                case 1303:
                    if (clickModal("Apply automatic cleanup")) next(1311);
                    break;
                case 1311:
                    if (!juce::Component::getCurrentlyModalComponent())
                    {
                        button(*centerWindow(), "Later")->triggerClick(); next(1304);
                    }
                    break;
                case 1304:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        if (health.needsPoll || health.completedOwners < 1 || centerWindow()) break;
                        check(health.maintenance.failure && health.completedOwners == 1
                            && !journal.getChildFile(firstOwner.toString()).exists() && !owners().empty()
                            && document->captureWorkspaceSnapshot()->sameContents(*centerImage) && juce::SHA256(source).toHexString() == sourceHash,
                            "confirmed automatic policy removes old inactive owner after View closes and preserves current owner/history");
                        client->showRecoveryCenterForTesting(); next(1305);
                    }
                    break;
                case 1305:
                    if (auto* center = centerWindow())
                        if (auto* action = button(*center, "Storage"); action && action->isEnabled())
                        { action->triggerClick(); next(1306); }
                    break;
                case 1306:
                    if (centerText("Recovery storage details").contains("keep inactive recovery records for at least 1 day."))
                    {
                        render(*centerWindow(), "recovery-retention-persisted.png");
                        button(*centerWindow(), "Retention...")->triggerClick(); next(1307);
                    }
                    break;
                case 1308:
                    if (clickModal("Apply automatic cleanup")) next(1309);
                    break;
                case 1309:
                    if (centerText("Recovery storage details").contains("keep inactive recovery records for at least 30 days."))
                    {
                        const auto* service = client->recoveryServiceForTesting();
                        if (!service || service->health().needsPoll) break;
                        check(service->health().completedOwners == 1 && service->health().maintenance.failure
                            && document->undo() && document->redo() && document->captureWorkspaceSnapshot()->sameContents(*centerImage)
                            && juce::SHA256(source).toHexString() == sourceHash && !engine->isPlaying(),
                            "native policy update restarts sweep without false removed-owner counts or history loss");
                        render(*centerWindow(), "recovery-retention-updated.png");
                        centerWindow()->closeButtonPressed(); expectedQuit = true; client->requestCloseApplication(); next(49);
                    }
                    break;
                case 1111:
                    if (storageReadEntered && storageReadEntered->wait(0))
                    {
                        auto* cancel = button(*centerWindow(), "Cancel scan"); check(cancel && cancel->isEnabled(), "real scan exposes cancellation");
                        cancel->triggerClick(); next(1112);
                    }
                    break;
                case 1112:
                    if (!button(*centerWindow(), "Cancel scan")->isEnabled())
                    { storageReadRelease->signal(); next(1113); }
                    break;
                case 1113:
                    if (centerText("Recovery storage details").contains("Scan cancelled."))
                    {
                        check(document->captureWorkspaceSnapshot()->sameContents(*centerImage) && juce::SHA256(source).toHexString() == sourceHash,
                            "native Cancel scan preserves complete current history and source");
                        auto* center = centerWindow();
                        dynamic_cast<RecoveryCenterComponent*>(center->getContentComponent())->setStorageReadBoundaryForTesting({});
                        render(*center, "recovery-storage-cancelled.png");
                        button(*center, "Records")->triggerClick(); next(1114);
                    }
                    break;
                case 1114:
                    if (auto* center = centerWindow())
                        if (auto* review = button(*center, "Review Discard"); review && review->isShowing() && review->isEnabled())
                        {
                            check(diagnosticChild(*center, "Recovery record details")->isShowing()
                                && !diagnosticChild(*center, "Recovery storage details")->isShowing(), "returning to Records restores the original selected record actions");
                            storageChecked = true; next(40);
                        }
                    break;
                case 1104:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        if (health.needsPoll || !std::any_of(health.protection.sessions.begin(), health.protection.sessions.end(),
                            [](const auto& session) { return session.active && session.protection.currentProtected; })) break;
                        check(!client->hasRecoveryCenterForTesting() && document->captureWorkspaceSnapshot()->sameContents(*centerImage)
                            && document->undo() && document->redo() && document->captureWorkspaceSnapshot()->sameContents(*centerImage),
                            "automatic maintenance leaves native complete history and subsequent protection intact");
                        render(*window, "recovery-automatic-terminal-cleanup.png");
                        client->showRecoveryCenterForTesting(); next(1103);
                    }
                    break;
                case 1103:
                    if (auto* center = centerWindow())
                        if (auto* records = combo(*center, "Recovery records"); records && records->isEnabled())
                        {
                            for (int i = 0; i < records->getNumItems(); ++i)
                            {
                                records->setSelectedItemIndex(i, juce::sendNotificationSync);
                                check(!centerText().contains(firstOwner.toString()), "reopened Center no longer lists the collected prior owner");
                            }
                            render(*center, "recovery-persisted-cleanup-complete.png"); center->closeButtonPressed();
                            expectedQuit = true; client->requestCloseApplication(); next(49);
                        }
                    break;
                case 60:
                    if (document->hasAudio() && waveform->isPeakCacheReady() && elapsed > 500.0) next(40);
                    break;
                case 80:
                    if (!centerText().contains("Metadata and history references checked")) break;
                    check(centerText().contains("Quarantined for this point and build") && document->captureWorkspaceSnapshot()->sameContents(*centerImage),
                        "Safe Inspect shows real interruption quarantine without changing current full history");
                    check(playlist->getNumEntries() == 1 && !recoveryRoot.getSiblingFile("RecoveryScratch").exists(), "Safe Inspect creates neither copy nor scratch");
                    render(*centerWindow(), "recovery-safe-inspect.png");
                    {
                        const auto bodies = journal.getChildFile(firstOwner.toString()).getChildFile("assets").findChildFiles(juce::File::findFiles, false);
                        check(!bodies.isEmpty(), "actual selected owner has stored audio for explicit verification");
                        verificationAsset = bodies.getFirst(); verificationHeld = verificationAsset.getSiblingFile(verificationAsset.getFileName() + ".verify-held");
                        check(verificationAsset.isAChildOf(fixture) && verificationAsset.moveFileTo(verificationHeld), "hide exactly one owned stored-audio fixture");
                        button(*centerWindow(), "Deep Verify audio")->triggerClick(); next(100);
                    }
                    break;
                case 100:
                    if (!centerText().contains("Audio verification stopped: asset_hash_or_read")) break;
                    check(centerText().contains("Stored audio bodies have NOT been fully verified") && playlist->getNumEntries() == 1
                        && document->captureWorkspaceSnapshot()->sameContents(*centerImage), "actual failed audio verification preserves current audio/history and never claims full readiness");
                    render(*centerWindow(), "recovery-audio-verification-missing.png");
                    {
                        const auto* center = dynamic_cast<RecoveryCenterComponent*>(centerWindow()->getContentComponent());
                        const auto observed = center->diagnosticObservation();
                        check(observed && !observed->operationId.isNull(), "actual failed inspection owns a real operation ID");
                        inspectionFailureOperation = observed->operationId;
                    }
                    check(verificationHeld.moveFileTo(verificationAsset), "repair only the exact held fixture before explicit verification retry");
                    button(*centerWindow(), "Deep Verify audio")->triggerClick(); next(101); break;
                case 101:
                    if (!centerText().contains("Stored audio verified for this point and its complete Undo/Redo history")) break;
                    {
                        const auto attempts = recovery::RecoveryAttemptStore(recovery::RecoveryStorageLayout(recoveryRoot).attemptDirectory(), firstOwner).inspect();
                        check(attempts.status && attempts.attempts.size() == 2 && playlist->getNumEntries() == 1
                            && !recoveryRoot.getSiblingFile("RecoveryScratch").exists(), "actual successful audio Deep Verify creates no restore marker, copy or scratch");
                    }
                    render(*centerWindow(), "recovery-audio-verification-complete.png");
                    button(*centerWindow(), "Check original")->triggerClick(); next(81); break;
                case 81:
                    if (!centerText().contains("Exact: original identity")) break;
                    button(*centerWindow(), "Deep Verify source")->triggerClick(); next(82); break;
                case 82:
                    if (!centerText().contains("Full source SHA-256: " + sourceHash)) break;
                    render(*centerWindow(), "recovery-source-deep-verify.png");
                    {
                        auto* points = combo(*centerWindow(), "Recovery safe points");
                        check(points && points->getNumItems() >= 3, "actual combo lists verified earlier history points");
                        points->setSelectedItemIndex(points->getNumItems() - 1, juce::sendNotificationSync); next(83);
                    }
                    break;
                case 83:
                    if (!centerText().contains("Undo steps: 1; Redo steps: 0")) break;
                    check(!centerText().contains("Quarantined for this point and build"), "quarantine does not leak to earlier exact point");
                    render(*centerWindow(), "recovery-earlier-point.png");
                    if (earlierMode) { button(*centerWindow(), "Restore Copy")->triggerClick(); next(71); break; }
                    combo(*centerWindow(), "Recovery safe points")->setSelectedItemIndex(0, juce::sendNotificationSync); next(84); break;
                case 84:
                    if (!centerText().contains("Quarantined for this point and build")) break;
                    {
                        const auto relocated = source.getSiblingFile("relocated-original.wav");
                        check(source.moveFileTo(relocated), "move only the owned original to exercise Missing and Locate");
                        source = relocated;
                        check(document->hasAudio() && document->hasUnsavedChanges() && document->canUndo()
                            && document->captureWorkspaceSnapshot()->hasUnsavedChanges() && !document->canSaveOverSourceFile(),
                            "moving the locator preserves admitted audio and dirty/history guards without offering overwrite of the missing path");
                    }
                    button(*centerWindow(), "Check original")->triggerClick(); next(85); break;
                case 85:
                    if (!centerText().contains("Missing: locate the original")) break;
                    button(*centerWindow(), "Locate original...")->triggerClick(); next(86); break;
                case 86:
                    if (chooseOwnedFile(source)) next(87);
                    break;
                case 87:
                    if (!centerText().contains("Relocated: matching file")) break;
                    render(*centerWindow(), "recovery-located-source.png");
                    button(*centerWindow(), "Export diagnostics...")->triggerClick(); next(88); break;
                case 88:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "Center opens the common diagnostic preview");
                            diagnosticReview = preview->getText();
                            const auto recovery = reviewedProvider(diagnosticReview, "recovery");
                            const auto fields = recovery["fields"];
                            const auto* center = dynamic_cast<RecoveryCenterComponent*>(centerWindow()->getContentComponent());
                            const auto observation = center->diagnosticObservation();
                            check(fields["recoveryFailureStage"]["value"].toString() == "center-audio-verification"
                                && recovery["latestFailure"]["operationId"].toString() == inspectionFailureOperation.toString()
                                && static_cast<int>(recovery["latestFailure"]["code"]) == static_cast<int>(recovery::Error::missingOrCorruptAsset)
                                && observation && observation->operationId != inspectionFailureOperation,
                                "actual missing-audio failure remains the shared latest capsule through successful audio recheck, Missing classification and Locate");
                            check(observation && fields["inspectionOperationId"]["value"].toString() == observation->operationId.toString()
                                && fields["inspectionSelectedHead"]["value"].toString() == observation->selectedHead.hash
                                && fields["inspectionSourceConfidence"]["value"].toString() == "relocated"
                                && static_cast<int>(fields["inspectionQuarantined"]["value"]) == 1
                                && static_cast<int64_t>(fields["inspectionSourceWindowBytes"]["value"]) > 0
                                && fields["inspectionAudioStatus"]["unavailable"].toString() == "not-applicable",
                                "actual selected inspection supplies exact operation, relocated classification and scoped quarantine without claiming audio verification");
                            check(!diagnosticReview.contains("providers/audio.json") && !diagnosticReview.contains("providers/codec.json")
                                && button(*dialog, "Recovery")->getToggleState() && !juce::Component::getCurrentlyModalComponent(),
                                "Center export opens a non-modal Recovery-only reviewed scope");
                            diagnosticWorkspace = document->captureWorkspaceSnapshot();
                            check(artifacts.getChildFile("recovery-inspection-review.txt").replaceWithText(diagnosticReview), "retain actual inspection preview");
                            render(*dialog, "recovery-inspection-review.png");
                            save->triggerClick(); next(888);
                        }
                    break;
                case 888:
                    if (chooseOwnedFile((centerDiagnosticRejectedFolders == 0 ? journal
                        : recovery::RecoveryStorageLayout(recoveryRoot).attemptDirectory()).getChildFile("diagnostics-must-not-enter-storage.zip"))) next(889);
                    break;
                case 889:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* status = dynamic_cast<juce::Label*>(diagnosticChild(*dialog, "Diagnostic status"));
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(status && status->getText().contains("outside recovery storage") && preview && preview->getText() == diagnosticReview
                                && !journal.getChildFile("diagnostics-must-not-enter-storage.zip").exists()
                                && !recovery::RecoveryStorageLayout(recoveryRoot).attemptDirectory().getChildFile("diagnostics-must-not-enter-storage.zip").exists(),
                                "the shared writer preserves both existing Recovery directory exclusions without changing the approved snapshot");
                            render(*dialog, centerDiagnosticRejectedFolders == 0 ? "recovery-diagnostics-journal-rejected.png" : "recovery-diagnostics-attempts-rejected.png");
                            ++centerDiagnosticRejectedFolders; save->triggerClick(); next(centerDiagnosticRejectedFolders < 2 ? 888 : 886);
                        }
                    break;
                case 886:
                    if (chooseOwnedFile(artifacts.getChildFile("recovery-inspection-reviewed.zip"))) next(89);
                    break;
                case 89:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            const auto file = artifacts.getChildFile("recovery-inspection-reviewed.zip");
                            check(static_cast<bool>(diagnostics::DiagnosticBundle::validate(file).status), "Center's actual reviewed bundle validates");
                            juce::ZipFile archive(file);
                            const auto index = archive.getIndexOfFileName("providers/recovery.json");
                            check(index >= 0 && archive.getIndexOfFileName("providers/audio.json") < 0, "published Center scope contains only Recovery");
                            std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(index));
                            check(input != nullptr, "read actual recovery inspection payload");
                            const auto text = input->readEntireStreamAsString();
                            check(diagnosticReview.contains(juce::JSON::toString(juce::JSON::parse(text), false))
                                && !text.contains(fixture.getFullPathName()) && !text.contains(source.getFileName())
                                && !text.contains("native-recovery-source.wav"), "published inspection is exactly reviewed and omits private paths and source names");
                            check(document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace)
                                && centerText().contains("Relocated: matching file"), "export preserves actual current history and the reviewed source needed for Restore Copy");
                            diagnosticWorkspace.reset(); dialog->closeButtonPressed();
                            button(*centerWindow(), "Restore Copy")->triggerClick(); next(90);
                        }
                    break;
                case 90:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Retry quarantined recovery once?")
                    { render(*modal, "recovery-retry-once-confirmation.png"); check(clickModal("Cancel"), "cancel explicit quarantine and relocated confirmation"); next(91); }
                    break;
                case 91:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    check(playlist->getNumEntries() == 1 && !recoveryRoot.getSiblingFile("RecoveryScratch").exists(), "cancelled Retry Once creates no restore job or scratch");
                    button(*centerWindow(), "Restore Copy")->triggerClick(); next(92); break;
                case 92:
                    if (clickModal("Retry Once")) next(71);
                    break;
                case 70:
                    if (auto* center = centerWindow())
                    {
                        if (!centerText("Recovery restore task details").contains("recovery_scratch_directory")) break;
                        check(playlist->getNumEntries() == 1 && document->captureWorkspaceSnapshot()->sameContents(*centerImage),
                            "actual failed restore has not changed current document or playlist");
                        render(*center, "recovery-restore-failure.png");
                        restoreDiagnosticFailure = dynamic_cast<RecoveryCenterComponent*>(center->getContentComponent())->selectedRestoreAttempt();
                        auto* diagnostics = button(*center, "Task diagnostics...");
                        check(diagnostics && diagnostics->isEnabled(), "failed restore exposes the common diagnostic action without an inspection");
                        diagnostics->triggerClick(); next(900);
                    }
                    break;
                case 900:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "restore task diagnostic preview exists");
                            diagnosticReview = preview->getText();
                            const auto recovery = reviewedProvider(diagnosticReview, "recovery");
                            const auto fields = recovery["fields"];
                            check(fields["restoreAttempt"]["value"].toString() == restoreDiagnosticFailure.toString()
                                && fields["restoreStage"]["value"].toString() == "failed"
                                && fields["restoreFailureStage"]["value"].toString() == "queued"
                                && fields["restoreReason"]["value"].toString() == "recovery_scratch_directory"
                                && fields["recoveryFailureStage"]["value"].toString() == "restore-storage"
                                && recovery["latestFailure"]["operationId"].toString() == restoreDiagnosticFailure.toString()
                                && fields["restoreLaunchId"]["value"].toString() != restoreDiagnosticFailure.toString()
                                && !juce::Uuid(fields["restoreLaunchId"]["value"].toString()).isNull(),
                                "actual scratch preparation failure supplies exact attempt, phase and separate launch ID");
                            check(button(*dialog, "Recovery")->getToggleState() && !diagnosticReview.contains("providers/audio.json")
                                && !diagnosticReview.contains("providers/codec.json") && !diagnosticReview.contains(source.getFileName())
                                && !diagnosticReview.contains(fixture.getFullPathName()), "task action preselects only Recovery and omits local source identity");
                            check(artifacts.getChildFile("restore-task-review.txt").replaceWithText(diagnosticReview), "retain actual restore task preview");
                            render(*dialog, "restore-task-review.png"); save->triggerClick(); next(901);
                        }
                    break;
                case 901:
                    if (chooseOwnedFile(artifacts.getChildFile("restore-task-reviewed.zip"))) next(902);
                    break;
                case 902:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            const auto file = artifacts.getChildFile("restore-task-reviewed.zip");
                            check(static_cast<bool>(diagnostics::DiagnosticBundle::validate(file).status), "actual restore diagnostic ZIP validates");
                            juce::ZipFile archive(file);
                            const auto index = archive.getIndexOfFileName("providers/recovery.json");
                            check(index >= 0, "actual restore provider is present");
                            std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(index));
                            check(input && diagnosticReview.contains(juce::JSON::toString(juce::JSON::parse(input->readEntireStreamAsString()), false))
                                && document->captureWorkspaceSnapshot()->sameContents(*centerImage) && playlist->getNumEntries() == 1,
                                "task export publishes the approved facts without retrying or changing current audio/history");
                            dialog->closeButtonPressed();
                            check(recoveryRoot.getSiblingFile("RecoveryScratch").deleteFile(), "remove only the known owned obstruction before retry");
                            auto* retry = button(*centerWindow(), "Retry restore"); check(retry && retry->isEnabled(), "failed restore still exposes explicit Retry");
                            retry->triggerClick(); next(71);
                        }
                    break;
                case 71:
                    if (centerText("Recovery restore task details").contains("recovery_scratch_directory")) break;
                    if (auto* center = centerWindow()) center->closeButtonPressed();
                    next(72); break;
                case 72:
                    {
                        const auto* queue = client->recoveryRestoreForTesting();
                        check(queue != nullptr, "closing Center preserves the actual restore queue");
                        const auto health = queue->health();
                        if (std::none_of(health.jobs.begin(), health.jobs.end(), [](const auto& job)
                            { return job.stage == recovery::RecoveryAttemptStage::ready; })) break;
                    }
                    if (inspectionMode)
                    {
                        const auto history = recovery::RecoveryAttemptStore(recovery::RecoveryStorageLayout(recoveryRoot).attemptDirectory(), firstOwner).inspect();
                        const auto successes = std::count_if(history.attempts.begin(), history.attempts.end(), [](const auto& item)
                        { return item.record.outcome == recovery::AttemptOutcome::succeeded; });
                        check(history.status && history.attempts.size() == 3 && successes == 1,
                            "cancelled Retry Once minted no marker; confirmed retry adds exactly one actual successful nonce");
                    }
                    check(!centerWindow() && !client->hasRecoveryCenterForTesting() && playlist->getNumEntries() == 2 && playlist->getCurrentIndex() == 0
                        && document->captureWorkspaceSnapshot()->sameContents(*centerImage) && !engine->isPlaying(),
                        "background Restore Copy adds unselected entry and preserves actual current full workspace");
                    check(juce::SHA256(source).toHexString() == sourceHash && files::nativeFile(journal.getChildFile(firstOwner.toString())).exists(),
                        "Restore Copy leaves source bytes and original recovery owner intact");
                    client->showRecoveryCenterForTesting(); next(1070); break;
                case 1070:
                    if (!centerText("Recovery restore task details").contains("Ready; full audio and history available")) break;
                    render(*centerWindow(), "recovery-restore-ready.png");
                    check(playlist->getEntry(1)->recovered != nullptr, "recovered entry has real workspace owner");
                    if (!restoreDiagnosticFailure.isNull())
                    { button(*centerWindow(), "Task diagnostics...")->triggerClick(); next(903); break; }
                    activatePlaylistRow(1); next(76); break;
                case 903:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "successful retry diagnostic preview exists");
                            const auto recovery = reviewedProvider(preview->getText(), "recovery"); const auto fields = recovery["fields"];
                            check(fields["restoreStage"]["value"].toString() == "ready"
                                && fields["restoreAttempt"]["value"].toString() != restoreDiagnosticFailure.toString()
                                && static_cast<int>(fields["restoreCoreAdmitted"]["value"]) == 1
                                && static_cast<int64_t>(fields["restoreResidentBytes"]["value"]) > 0
                                && recovery["latestFailure"]["operationId"].toString() == restoreDiagnosticFailure.toString()
                                && fields["restoreFailureStage"]["unavailable"].toString() == "not-applicable",
                                "successful real retry is a new admitted attempt while the original failure remains immutable");
                            render(*dialog, "restore-task-retry-ready.png"); dialog->closeButtonPressed();
                            activatePlaylistRow(1); next(76);
                        }
                    break;
                case 76:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Unsaved Changes")
                    {
                        render(*modal, "recovery-switch-unsaved-confirmation.png");
                        check(clickModal("Cancel"), "actual playlist Cancel protects current unsaved edit"); next(77);
                    }
                    break;
                case 77:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    check(playlist->getCurrentIndex() == 0 && document->captureWorkspaceSnapshot()->sameContents(*centerImage),
                        "cancelled selection leaves the full original workspace unchanged");
                    check(document->undo() && !document->hasUnsavedChanges() && document->canRedo(),
                        "explicit Undo returns ordinary document to clean while preserving redoable edit");
                    activatePlaylistRow(1); next(73); break;
                case 73:
                    if (!document->getState()->info.recoveryBacked || !waveform->isPeakCacheReady()) break;
                    check(document->requiresRecoverySaveAs() && document->hasUnsavedChanges() && document->canUndo()
                        && document->canRedo() == !earlierMode, "selected recovered Document has SaveAs guard and the selected point's actual history stacks");
                    check(std::abs(document->getState()->envelopePoints.front().gain - 0.35f) < 1.0e-6f,
                        "restored current edit is the last safe state");
                    check(document->undo() && std::abs(document->getState()->envelopePoints.front().gain - 1.0f) < 1.0e-6f,
                        "real restored Undo returns previous original audio envelope");
                    check(document->redo(), "actual restored redo returns to selected point");
                    if (earlierMode)
                        check(!document->canRedo() && std::abs(document->getState()->envelopePoints.front().gain - 0.35f) < 1.0e-6f,
                            "earlier restore has no future .63 edit from newer journal points");
                    else
                    {
                        check(document->redo() && std::abs(document->getState()->envelopePoints.front().gain - 0.63f) < 1.0e-6f,
                            "real restored Redo retains the future edit");
                        check(document->undo(), "return recovered copy to initially selected state");
                    }
                    {
                        const auto codec = captureCurrentProvider("codec");
                        check(codec["fields"]["selectedReader"]["value"].toString() == "verified-recovery-pcm"
                            && codec["fields"]["dependencyIdentity"]["value"].toString() == "SingleEditor-built-in",
                            "actual Restore Copy reports its owned recovery reader rather than the historical source format");
                        check(artifacts.getChildFile("recovered-reader-observation.json").replaceWithText(juce::JSON::toString(codec)),
                            "retain the final recovered reader snapshot");
                        juce::AudioBuffer<float> output(2, 64);
                        check(engine->renderDocumentBlockForDiagnostics(32, output), "actual native recovered source renders without a device");
                        check(std::abs(output.getSample(0, 0)) > 0.0001f, "native recovered samples are real non-silent fixture audio");
                    }
                    render(*window, "recovery-restored-document.png");
                    if (restoreSaveMode)
                    {
                        check(document->getState()->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::recoveryStream
                            && document->getState()->streamingReader && source.existsAsFile(),
                            "native Save regression exercises recovery streaming with the different historical source still present");
                        restoredSaveState = document->getState();
                        restoredSaveHistoryPcm = restoredDspHistoryPcm();
                        check(client->keyPressed(juce::KeyPress('s', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0)),
                            "actual native Ctrl+Shift+S selects explicit Save As for the protected recovered copy");
                        next(1080); break;
                    }
                    if (bankScenario == "bank-fallback-recovered")
                    {
                        packedReturnedEntry = playlist->getCurrentEntry()->id;
                        videoOriginalState = document->getState(); retainPackedSelection();
                        check(engine->renderDocumentBlockForDiagnostics(0, videoOriginalPcm), "capture actual recovered PCM before bank fallback");
                        check(document->redo(), "execute original recovered future state before fallback");
                        juce::AudioBuffer<float> future(2, 128);
                        check(engine->renderDocumentBlockForDiagnostics(0, future), "capture actual recovered future PCM");
                        fallbackRedoPcm.push_back(std::move(future));
                        check(document->undo(), "return to recovered current point before fallback");
                        fallbackHistory = document->captureCommittedState(document->getMediaIdentity().document);
                        check(fallbackHistory && !fallbackHistory->undo.empty() && !fallbackHistory->redo.empty(), "real Restore Copy supplies both complete history stacks");
                        fallbackRecoveryOrigin = document->getRecoveryOrigin();
                        check(fallbackRecoveryOrigin.has_value(), "actual restored workspace has an exact recovery origin before fallback");
                        centerWindow()->closeButtonPressed();
                        packedBankFile = fixture.getChildFile("disguised-bank.wav");
                        juce::MemoryOutputStream broken;
                        broken.write("BKHD", 4); broken.writeInt(8); broken.writeInt(128); broken.writeInt(7);
                        broken.write("HIRC", 4); broken.writeInt(512);
                        check(packedBankFile.replaceWithData(broken.getData(), broken.getDataSize()), "create actual malformed bank after Restore Copy");
                        client->openInitialFile(packedBankFile); next(660); break;
                    }
                    if (packedRecoveredMode)
                    {
                        packedReturnedEntry = playlist->getCurrentEntry()->id;
                        videoOriginalState = document->getState();
                        retainPackedSelection(); centerWindow()->closeButtonPressed();
                        client->openInitialFile(videoSource); next(300); break;
                    }
                    activatePlaylistRow(0); next(74); break;
                case 1080:
                    if (chooseOwnedFile(artifacts.getChildFile("recovered-dsp-saved.wav"))) next(1081);
                    break;
                case 1081:
                    if (document->hasUnsavedChanges() || document->requiresRecoverySaveAs()
                        || document->getFileInfo().file != artifacts.getChildFile("recovered-dsp-saved.wav")) break;
                    verifyNativeRecoveredSave();
                    render(*window, "recovery-saved-dsp-document.png");
                    restoredSaveState.reset(); restoredSaveHistoryPcm.clear();
                    activatePlaylistRow(0); next(74); break;
                case 74:
                    if (document->getState()->info.recoveryBacked || !waveform->isPeakCacheReady()) break;
                    check(!document->hasUnsavedChanges() && document->canRedo() && document->redo()
                        && std::abs(document->getState()->envelopePoints.front().gain - 0.57f) < 1.0e-6f && document->canUndo()
                        && juce::SHA256(source).toHexString() == sourceHash && !engine->isPlaying(),
                        "ordinary workspace retains its own edit/history and original source after full recovered navigation");
                    centerWindow()->closeButtonPressed(); expectedQuit = true; client->requestCloseApplication(); next(75); break;
                case 75:
                    if (inspectionMode && !earlierMode)
                        if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery still pending")
                        {
                            check(modal->getDescription().contains("Recovery has not finished") && !modal->getDescription().contains("Saved audio is safe"),
                                "a moved live source cannot be falsely reported as fully protected at close");
                            check(modal->getDescription().contains(inspectedCloseFailure
                                    ? "audio source needed by recovery has changed" : "audio source needed by recovery is unavailable or busy")
                                && modal->getDescription().contains("complete Undo/Redo history still needs recovery protection"),
                                "close explains actual source loss and the limits of Save As without discarding history");
                            render(*modal, inspectedCloseFailure ? "recovery-changed-live-source-close.png" : "recovery-moved-live-source-close.png");
                            if (inspectedCloseFailure) { check(clickModal("Exit Anyway"), "explicit exit preserves existing records after diagnosed live-source loss"); next(97); }
                            else { check(clickModal("Cancel"), "cancel close to inspect actual moved-source protection failure"); next(93); }
                            break;
                        }
                    clickModal("Discard");
                    break;
                case 93:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    clickStatus(); next(94); break;
                case 94:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery")
                    {
                        check(modal->getDescription().contains("audio source needed by recovery is unavailable or busy")
                            && modal->getDescription().contains("same source is accessible again")
                            && modal->getDescription().contains("Existing recovery points are kept"),
                            "recovery actions explain the real source failure before requiring Diagnostics");
                        render(*modal, "recovery-moved-live-source-actions.png");
                        if (clickModal("Diagnostics")) next(95);
                    }
                    break;
                case 95:
                    if (auto* dialog = diagnosticsWindow())
                    {
                        auto* save = button(*dialog, "Save reviewed bundle...");
                        auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                        if (!save || !save->isEnabled() || !preview) break;
                        const auto recovery = reviewedProvider(preview->getText(), "recovery");
                        check(recovery["fields"]["recoveryProtectionReason"]["value"].toString() == "source_lease_unavailable",
                            "common diagnostics preserve the actual unavailable original read lease reason");
                        render(*dialog, "recovery-moved-live-source-diagnostics.png");
                        inspectedCloseFailure = true; dialog->closeButtonPressed(); next(96);
                    }
                    break;
                case 96:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    {
                        sourceRetryHistory = document->captureCommittedState(document->getMediaIdentity().document);
                        check(sourceRetryHistory.has_value(), "capture the actual post-close-cancellation document before Retry");
                        const auto oldPath = fixture.getChildFile("native-recovery-source.wav");
                        check(!oldPath.existsAsFile()
                            && source.copyFileTo(oldPath), "recreate only the owned old locator with byte-identical but physically different audio");
                        check(juce::SHA256(oldPath).toHexString() == sourceHash,
                            "replacement source fixture has identical audio bytes");
                        clickStatus(); next(1090);
                    }
                    break;
                case 1090:
                    if (clickModal("Retry")) next(1091);
                    break;
                case 1091:
                    if (const auto* service = client->recoveryServiceForTesting())
                    {
                        const auto health = service->health();
                        bool sourceChanged = false;
                        const auto inspect = [&](const auto& epoch)
                        { sourceChanged = sourceChanged || (!epoch.writer.workerRunning && epoch.failure.code == "audio_source_identity_changed"); };
                        for (const auto& session : health.protection.sessions)
                        {
                            inspect(session.protection.current);
                            for (const auto& epoch : session.protection.retired) inspect(epoch);
                        }
                        if (!sourceChanged || juce::Component::getCurrentlyModalComponent()) break;
                        const auto historyUnchanged = [&]
                        {
                            const auto snapshot = document->captureCommittedState(sourceRetryHistory->identity.document);
                            return snapshot && snapshot->state == sourceRetryHistory->state
                                && snapshot->undo == sourceRetryHistory->undo && snapshot->redo == sourceRetryHistory->redo;
                        };
                        check(historyUnchanged(), "failed Retry preserves the exact current document and both complete history stacks");
                        check(document->canUndo() && document->undo() && document->canRedo() && document->redo() && historyUnchanged(),
                            "Undo and Redo remain executable after failed source retry and return to the captured document");
                        clickStatus(); next(1092);
                    }
                    break;
                case 1092:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery")
                    {
                        check(modal->getDescription().contains("audio source needed by recovery has changed")
                            && modal->getDescription().contains("Retry cannot accept a different source version"),
                            "source-version conflict explains why another Retry cannot accept replacement audio");
                        render(*modal, "recovery-changed-live-source-actions.png");
                        check(clickModal("Close"), "close source conflict explanation without changing the document"); next(1093);
                    }
                    break;
                case 1093:
                    if (juce::Component::getCurrentlyModalComponent()) break;
                    client->requestCloseApplication(); next(75); break;
                case 97:
                    break;
                case 920:
                    if (clickModal("Diagnostics")) next(921);
                    break;
                case 921:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            auto* preview = dynamic_cast<juce::TextEditor*>(diagnosticChild(*dialog, "Diagnostic snapshot preview"));
                            check(preview != nullptr, "unavailable-root diagnostic preview exists");
                            diagnosticReview = preview->getText();
                            const auto result = reviewedProvider(diagnosticReview, "recovery");
                            const auto fields = result["fields"], failure = result["latestFailure"];
                            check(fields["recoveryFailureStage"]["value"].toString() == "service-configuration"
                                && fields["recoveryDispositionReason"]["value"].toString() == "service_root"
                                && static_cast<int>(failure["code"]) == static_cast<int>(recovery::Error::invalidPacket)
                                && !juce::Uuid(failure["operationId"].toString()).isNull()
                                && fields["recoveryFailureOwner"]["unavailable"].toString() == "not-reported",
                                "actual service setup failure has a typed operation and no fabricated owner");
                            check(button(*dialog, "Recovery")->getToggleState() && !button(*dialog, "Audio device")->getToggleState()
                                && !diagnosticReview.contains("providers/audio.json") && !diagnosticReview.contains(source.getFileName())
                                && !diagnosticReview.contains(fixture.getFullPathName()), "setup failure uses Recovery-selected common preview and default privacy");
                            render(*dialog, "service-setup-diagnostic-preview.png");
                            save->triggerClick(); next(922);
                        }
                    break;
                case 922:
                    if (chooseOwnedFile(artifacts.getChildFile("service-setup-reviewed.zip"))) next(923);
                    break;
                case 923:
                    if (auto* dialog = diagnosticsWindow())
                        if (auto* save = button(*dialog, "Save reviewed bundle..."); save && save->isEnabled())
                        {
                            const auto file = artifacts.getChildFile("service-setup-reviewed.zip");
                            check(static_cast<bool>(diagnostics::DiagnosticBundle::validate(file).status), "actual setup diagnostic ZIP validates");
                            juce::ZipFile archive(file); const auto index = archive.getIndexOfFileName("providers/recovery.json");
                            check(index >= 0, "setup ZIP contains actual Recovery section");
                            std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(index));
                            check(input && diagnosticReview.contains(juce::JSON::toString(juce::JSON::parse(input->readEntireStreamAsString()), false)),
                                "setup diagnostic publication exactly matches approved preview");
                            check(document->captureWorkspaceSnapshot()->sameContents(*diagnosticWorkspace) && !recoveryRoot.exists()
                                && !fixture.getChildFile("journals").exists(), "setup diagnostics preserve current history and do not create a relative recovery root");
                            diagnosticWorkspace.reset(); dialog->closeButtonPressed();
                            client->requestCloseApplication(); next(30);
                        }
                    break;
                case 30:
                    if (clickModal("Discard")) next(31);
                    break;
                case 31:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery still pending")
                    {
                        check(modal->getDescription().contains("Recovery has not finished")
                            && !modal->getDescription().contains("Saved audio is safe"),
                            "missing recovery root is not presented as saved-owner cleanup");
                        render(*modal, "invalid-root-close-client-render.png");
                        check(clickModal("Cancel"), "Cancel unavailable-root close"); next(32);
                    }
                    break;
                case 32:
                    if (juce::Component::getCurrentlyModalComponent() || elapsed < 250.0) break;
                    check(document->hasUnsavedChanges() && document->canUndo() && statusVisible()
                        && !recoveryRoot.exists() && !fixture.getChildFile("journals").exists(),
                        "unavailable-root Cancel retains changes/history and never writes a relative root");
                    client->requestCloseApplication(); next(33); break;
                case 33:
                    if (clickModal("Discard")) next(34);
                    break;
                case 34:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Recovery still pending")
                    { expectedQuit = true; check(clickModal("Exit Anyway"), "explicit unavailable-root exit"); next(35); }
                    break;
                case 300:
                    if (document->getFileInfo().file == videoSource && waveform->hasWaveform())
                    {
                        check(!document->getState()->playbackAvailability.isRandomAccess() && document->getState()->getPlayableSampleRange().getEnd() == 48000,
                            "native adopts and displays a real one-second prefix before full decode");
                        juce::AudioBuffer<float> pcm(1, 64);
                        check(engine->renderDocumentBlockForDiagnostics(32, pcm) && std::abs(pcm.getSample(0, 2) - 0.25f) < 1.0e-6f,
                            "native real engine can play prefix without opening audio device");
                        render(*window, "wem-real-prefix.png");
                        if (packedCloseMode) { expectedQuit = true; client->requestCloseApplication(); next(340); break; }
                        if (packedEditedMode)
                        {
                            check(document->moveEnvelopePoint(0, 0, 0.61f) && document->moveEnvelopePoint(0, 0, 0.22f) && document->undo(),
                                "make real current/Undo/Redo while WEM still decoding");
                            packedEditedState = document->getState();
                        }
                        if (packedBankMode) check(packedBankFile.moveFileTo(fixture.getChildFile("moved-return.bnk")), "move actual original bank while WEM is pending");
                        check(videoSource.getSiblingFile(videoSource.getFileName() + ".finish").replaceWithText("finish"), "release real child to late failure"); next(301);
                    }
                    break;
                case 301:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "WEM audio unavailable")
                    {
                        if (packedRecoveredMode)
                        {
                            check(playlist->getCurrentEntry() && playlist->getCurrentEntry()->id == packedReturnedEntry
                                && playlist->getCurrentEntry()->recovered && document->getState()->info.recoveryBacked
                                && document->requiresRecoverySaveAs() && document->canUndo() && document->canRedo()
                                && document->getState()->streamingReader == videoOriginalState->streamingReader,
                                "late WEM failure restores exact recovered playlist owner and reader with full history");
                            checkPackedSelection();
                            check(document->redo() && std::abs(document->getState()->envelopePoints.front().gain - 0.63f) < 1.0e-6f
                                && document->undo() && document->undo() && document->redo(), "returned real recovered history executes both directions");
                            render(*window, "wem-returned-recovered.png"); check(clickModal("Keep current audio"), "dismiss failure after recovered return"); next(353); break;
                        }
                        if (packedEditedMode)
                        {
                            check(document->getState() == packedEditedState && document->canUndo() && document->canRedo()
                                && document->redo() && document->undo() && document->getState() == packedEditedState,
                                "late failure cannot overwrite current edits even when Undo decreased generation");
                            render(*window, "wem-late-failure-kept-edits.png"); check(clickModal("Keep current audio"), "keep edited WEM after late failure"); next(355); break;
                        }
                        if (packedBankMode)
                        {
                            check(bank->getState() == packedBankState && bank->getSelectedMediaId() == uint32_t{99}
                                && !bank->hasBank() && !bank->getState()->isSourceCurrent()
                                && document->getState()->audioBuffer == videoOriginalState->audioBuffer
                                && playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == packedBankFile,
                                "native returns parsed selection and actual preview without accepting relocated source");
                            juce::AudioBuffer<float> pcm(2, 64);
                            check(engine->renderDocumentBlockForDiagnostics(32, pcm)
                                && pcm.getSample(0, 0) == videoOriginalState->audioBuffer->getSample(0, 32),
                                "returned native bank preview still renders exact retained PCM after original locator moved");
                            render(*window, "wem-returned-bank.png"); check(clickModal("Keep current audio"), "dismiss failure after bank return"); next(353); break;
                        }
                        checkPackedOriginal(); check(videoInvocations() == 1 && button(*modal, "Retry once"), "actual late failure offers one explicit retry");
                        if (packedBudgetMode)
                        {
                            check(button(*modal, "Retry with Expanded Budget"), "actual WEM quota offers finite reviewed expansion");
                            render(*modal, "wem-budget-review.png"); check(clickModal("Retry with Expanded Budget"), "expand actual WEM output budget"); next(320); break;
                        }
                        render(*modal, "wem-late-failure.png"); check(clickModal("Retry once"), "retry actual WEM once"); next(302);
                    }
                    break;
                case 302:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "WEM audio unavailable" && elapsed > 300)
                    {
                        check(videoInvocations() == 2, "WEM retry must produce exactly two actual child invocations");
                        checkPackedOriginal(); check(!button(*modal, "Retry once"), "actual second failure opens WEM session fuse");
                        render(*modal, "wem-fused.png"); check(clickModal("Export diagnostics..."), "explicit native WEM diagnostic export"); next(303);
                    }
                    break;
                case 303:
                    if (chooseReviewedMediaDiagnosticFile("wem-diagnostics")) next(304);
                    break;
                case 304:
                    if (finishReviewedMediaDiagnosticFile("wem-diagnostics"))
                    {
                        videoSource = fixture.getChildFile("cancel.wem"); check(videoSource.replaceWithText("packed-fixture:hang"), "create native cancellation source");
                        client->openInitialFile(videoSource); next(305);
                    }
                    break;
                case 305:
                    if (document->getFileInfo().file == videoSource && waveform->hasWaveform() && clickModal("Cancel WEM audio")) next(306);
                    break;
                case 306:
                    if (elapsed > 250 && !juce::Component::getCurrentlyModalComponent() && document->getFileInfo().file == source)
                    {
                        checkPackedOriginal(); check(videoInvocations() == 3, "cancellation launches no automatic retry"); checkVideoChildrenExited();
                        videoSource = fixture.getChildFile("success.wem"); check(videoSource.replaceWithText("packed-fixture:success"), "create successful native progressive source");
                        client->openInitialFile(videoSource); next(307);
                    }
                    break;
                case 307:
                    if (document->getFileInfo().file == videoSource && waveform->hasWaveform())
                    {
                        check(document->moveEnvelopePoint(0, 0, 0.7f) && document->moveEnvelopePoint(0, 0, 0.4f)
                            && document->undo() && document->canUndo() && document->canRedo(), "create actual history before native decode terminal");
                        packedEditedState = document->getState();
                        check(videoSource.getSiblingFile(videoSource.getFileName() + ".finish").replaceWithText("finish"), "release native successful decoder"); next(308);
                    }
                    break;
                case 308:
                    if (!juce::Component::getCurrentlyModalComponent() && document->getState()->playbackAvailability.isRandomAccess() && packedHistoryProtected())
                    {
                        check(document->getState() == packedEditedState && document->canUndo() && document->canRedo()
                            && playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == videoSource,
                            "native completion keeps exact edited State/full history and the correct selected playlist owner");
                        juce::AudioBuffer<float> pcm(1, 64);
                        check(engine->renderDocumentBlockForDiagnostics(96000, pcm) && pcm.getSample(0, 0) > 0.1f, "actual native engine renders beyond original prefix after seal");
                        check(document->redo() && document->undo() && document->undo() && !document->hasUnsavedChanges() && document->canRedo(),
                            "native full Undo/Redo survives decode completion and returns to clean original");
                        render(*window, "wem-complete-history.png"); next(309);
                    }
                    break;
                case 309:
                    if (elapsed > 700)
                    {
                        checkVideoChildrenExited(); expectedQuit = true; client->requestCloseApplication(); next(310);
                    }
                    break;
                case 320:
                    if (!juce::Component::getCurrentlyModalComponent() && document->getFileInfo().file == videoSource && document->getState()->playbackAvailability.isRandomAccess())
                    {
                        check(videoInvocations() == 2 && !engine->isPlaying() && playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == videoSource,
                            "expanded WEM retry admits validated audio under the correct playlist owner without auto-play");
                        check(document->moveEnvelopePoint(0, 0, 0.27f) && document->undo() && document->canRedo(), "expanded WEM retains real edited history for protection");
                        render(*window, "wem-expanded-complete.png"); next(309);
                    }
                    break;
                case 330:
                    if (!juce::Component::getCurrentlyModalComponent()) { client->openInitialFile(source); next(331); }
                    break;
                case 331:
                    if (document->hasAudio() && document->getFileInfo().file == source && waveform->isPeakCacheReady())
                    {
                        check(videoInvocations() == 1, "ordinary audio opens after failed startup WEM without decoder retry");
                        checkVideoChildrenExited(); render(*window, "ordinary-after-wem-startup.png"); expectedQuit = true; client->requestCloseApplication(); next(332);
                    }
                    break;
                case 350:
                    if (bank->hasBank())
                    {
                        auto* list = findComponent<BnkAudioListViewComponent>(*window);
                        check(list && list->onMediaActivated, "actual bank media activation route exists");
                        list->onMediaActivated(99);
                        check(bank->getSelectedMediaId() == uint32_t{99}, "actual bank route selects the requested media before asynchronous preparation");
                        next(352);
                    }
                    break;
                case 352:
                    if (document->getState()->audioBuffer && document->getFileInfo().file == packedBankFile
                        && document->getFileInfo().numSamples == 48000 && !juce::Component::getCurrentlyModalComponent())
                    {
                        check(bank->getSelectedMediaId() == uint32_t{99}, "actual asynchronous native bank route adopts the complete selected PCM preview");
                        packedBankCache = bank->getEmbeddedMediaCacheFile(99);
                        packedBankCacheHash = juce::SHA256(packedBankCache).toHexString(); next(351);
                    }
                    break;
                case 351:
                    if (bank->getSelectedMediaId() == uint32_t{99} && document->hasAudio() && elapsed > 100)
                    {
                        juce::AudioBuffer<float> pcm(2, 64);
                        check(engine->renderDocumentBlockForDiagnostics(32, pcm) && std::abs(pcm.getSample(0, 0)) > 0.0001f,
                            "real bank preview renders PCM while its center presents the bank list");
                        packedBankState = bank->getState(); videoOriginalState = document->getState();
                        client->openInitialFile(videoSource); next(300);
                    }
                    break;
                case 353:
                    if (!juce::Component::getCurrentlyModalComponent())
                    {
                        checkVideoChildrenExited();
                        if (packedBankMode) { client->openInitialFile(source); next(358); break; }
                        expectedQuit = true; client->requestCloseApplication(); next(75);
                    }
                    break;
                case 355:
                    if (!juce::Component::getCurrentlyModalComponent())
                    {
                        check(document->undo() && !document->hasUnsavedChanges(), "explicit Undo keeps old WEM history but permits new activation");
                        videoSource = fixture.getChildFile("superseded.wem");
                        check(videoSource.replaceWithText("packed-fixture:hang"), "create active decoder to supersede");
                        client->openInitialFile(videoSource); next(357);
                    }
                    break;
                case 356:
                    if (document->getFileInfo().file == source && waveform->isPeakCacheReady() && elapsed > 500)
                    {
                        check(!juce::Component::getCurrentlyModalComponent() && videoInvocations() == 2, "newer ordinary activation remains selected with no stale optional result/retry");
                        checkVideoChildrenExited(); expectedQuit = true; client->requestCloseApplication(); next(75);
                    }
                    break;
                case 357:
                    if (document->getFileInfo().file == videoSource && waveform->hasWaveform())
                    { client->openInitialFile(source); next(356); }
                    break;
                case 358:
                    if (document->getFileInfo().file == source && waveform->isPeakCacheReady())
                    {
                        check(bank->getBankInfo().file == juce::File(), "ordinary activation clears retained parsed bank even when its locator is missing");
                        expectedQuit = true; client->requestCloseApplication(); next(75);
                    }
                    break;
                case 200:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable")
                    {
                        check(videoInvocations() == 1, "first video failure launches exactly one real child without fallback");
                        checkVideoOriginal();
                        check(button(*modal, "Retry once") && !button(*modal, "Retry with Expanded Budget"), "decoder exit allows explicit retry but no quota expansion");
                        render(*modal, "video-first-failure.png"); check(clickModal("Retry once"), "click actual video retry"); next(201);
                    }
                    break;
                case 201:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable" && videoInvocations() == 2)
                    {
                        checkVideoOriginal(); check(!button(*modal, "Retry once"), "second matching failure opens the session circuit");
                        render(*modal, "video-session-fused.png"); check(clickModal("Keep current audio"), "keep current after circuit opens"); next(202);
                    }
                    break;
                case 202:
                    if (!juce::Component::getCurrentlyModalComponent()) { client->openInitialFile(videoSource); next(203); }
                    break;
                case 203:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable")
                    {
                        check(videoInvocations() == 2 && !button(*modal, "Retry once"), "reopen does not bypass session fuse or launch another child");
                        checkVideoOriginal(); check(clickModal("Export diagnostics..."), "explicitly export only local typed video diagnostics"); next(204);
                    }
                    break;
                case 204:
                    if (chooseReviewedMediaDiagnosticFile("video-diagnostics")) next(205);
                    break;
                case 205:
                    if (finishReviewedMediaDiagnosticFile("video-diagnostics"))
                    {
                        check(videoSource.appendText(" changed physical length"), "change actual source identity");
                        client->openInitialFile(videoSource); next(206);
                    }
                    break;
                case 206:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable")
                    {
                        check(videoInvocations() == 3 && button(*modal, "Retry once"), "changed source gets one fresh actual decode and fresh retry");
                        checkVideoOriginal(); check(clickModal("Keep current audio"), "close changed-source failure"); next(207);
                    }
                    break;
                case 207:
                    if (!juce::Component::getCurrentlyModalComponent())
                    {
                        videoSource = fixture.getChildFile("hang.mp4"); check(videoSource.replaceWithText("hang"), "create private hanging source");
                        client->openInitialFile(videoSource); next(208);
                    }
                    break;
                case 208:
                    if (videoInvocations() == 4 && clickModal("Cancel video audio")) next(209);
                    break;
                case 209:
                    if (elapsed > 1000.0 && !juce::Component::getCurrentlyModalComponent())
                    {
                        checkVideoOriginal(); check(videoInvocations() == 4, "cancel does not automatically retry");
                        checkVideoChildrenExited();
                        render(*window, "video-cancel-kept-document.png");
                        check(document->redo() && std::abs(document->getState()->envelopePoints.front().gain - 0.43f) < 0.001f
                            && document->undo(), "actual preserved Redo and Undo still execute after all failures and cancellation");
                        expectedQuit = true; client->requestCloseApplication(); next(210);
                    }
                    break;
                case 220:
                    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal && modal->getName() == "Video audio unavailable")
                    {
                        checkVideoOriginal(); check(videoInvocations() == 1 && button(*modal, "Retry with Expanded Budget"), "actual output quota grants bounded expanded profile");
                        render(*modal, "video-budget-review.png"); check(clickModal("Retry with Expanded Budget"), "explicit expanded-budget choice"); next(221);
                    }
                    break;
                case 221:
                    if (document->getFileInfo().file == videoSource && document->hasAudio() && !juce::Component::getCurrentlyModalComponent())
                    {
                        check(videoInvocations() == 2 && !engine->isPlaying() && document->getFileInfo().numSamples == 48000
                            && playlist->getCurrentEntry() && playlist->getCurrentEntry()->file == videoSource,
                            "real expanded retry validates and publishes complete playable video audio without auto-play");
                        check(document->getState()->sourceFileWitness && document->getState()->sourceFileWitness->file == document->getState()->readerSourceFile
                            && document->getState()->readerSourceFile != videoSource
                            && !document->captureCommittedState(document->getMediaIdentity().document)->sourceVersion,
                            "actual cache backing has read facts without impersonating original video source evidence");
                        check(document->moveEnvelopePoint(0, 0, 0.27f), "edit actual video audio");
                        videoOriginalState = document->getState();
                        client->openInitialFile(videoSource); next(222);
                    }
                    break;
                case 222:
                    if (elapsed > 500.0)
                    {
                        check(!juce::Component::getCurrentlyModalComponent() && videoInvocations() == 2
                            && document->getState() == videoOriginalState && document->canUndo(), "same active video open preserves live edits/history without decoding or consuming retry");
                        check(document->undo() && document->canRedo(), "video Undo/Redo is real after successful candidate admission");
                        render(*window, "video-expanded-success.png"); expectedQuit = true; client->requestCloseApplication(); next(223);
                    }
                    break;
                case 230:
                    if (!juce::Component::getCurrentlyModalComponent()) { client->openInitialFile(source); next(231); }
                    break;
                case 231:
                    if (document->hasAudio() && document->getFileInfo().file == source && waveform->isPeakCacheReady())
                    {
                        check(videoInvocations() == 1 && !engine->isPlaying(), "ordinary open remains usable after startup video failure without optional retry");
                        checkVideoChildrenExited(); render(*window, "ordinary-after-video-startup-failure.png");
                        expectedQuit = true; client->requestCloseApplication(); next(232);
                    }
                    break;
                default: break;
            }
        }
        catch (const std::exception& e) { fail(e.what()); }
    }

    int videoInvocations() const
    {
        auto lines = decoderPids();
        lines.removeEmptyStrings(); return lines.size();
    }
    juce::StringArray decoderPids() const
    {
        if (!packedMode && bankScenario.isEmpty()) return juce::StringArray::fromLines(fixture.getChildFile("video-invocations.txt").loadFileAsString());
        juce::StringArray lines;
        for (const auto& file : fixture.findChildFiles(juce::File::findFiles, false, "*.wem.pids"))
            lines.addArray(juce::StringArray::fromLines(file.loadFileAsString()));
        return lines;
    }
    void checkVideoChildrenExited()
    {
#if JUCE_WINDOWS
        const auto lines = decoderPids();
        for (const auto& line : lines)
        {
            if (line.isEmpty()) continue;
            if (packedMode || bankScenario.isNotEmpty())
            {
                check(singleeditor::tests::decoderProcessExited(line), "actual owned decoder identity has exited after cancellation/failure");
                continue;
            }
            auto handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(line.getLargeIntValue()));
            if (!handle) continue;
            const auto done = WaitForSingleObject(handle, 0); CloseHandle(handle);
            check(done == WAIT_OBJECT_0, "actual owned decoder PID has exited after cancellation/failure");
        }
#endif
    }
    void checkVideoOriginal()
    {
        check(document->getState() == videoOriginalState && document->hasAudio() && document->canRedo()
            && !document->hasUnsavedChanges() && !engine->isPlaying(), "failed optional capability preserves exact current image and redoable history");
        juce::AudioBuffer<float> now(2, 128);
        check(engine->renderDocumentBlockForDiagnostics(0, now), "existing real engine still reads original PCM");
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
            check(now.getSample(c, i) == videoOriginalPcm.getSample(c, i), "failed optional decoder leaves original actual PCM unchanged");
    }
    void checkPackedOriginal(bool exerciseHistory = true)
    {
        if (exerciseHistory) checkPackedSelection();
        check(document->getFileInfo().file == source && document->canRedo() && !document->hasUnsavedChanges()
            && document->getState()->streamingReader == videoOriginalState->streamingReader
            && document->getState()->memoryMappedReader == videoOriginalState->memoryMappedReader, "WEM return restores original reader and complete history without disk reload");
        if (exerciseHistory) check(document->redo() && std::abs(document->getState()->envelopePoints.front().gain - 0.43f) < 0.001f && document->undo(),
            "real original Redo and Undo execute after WEM return");
        juce::AudioBuffer<float> pcm(2, 128); check(engine->renderDocumentBlockForDiagnostics(0, pcm), "returned native engine renders original PCM");
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
            check(pcm.getSample(c, i) == videoOriginalPcm.getSample(c, i), "returned native original PCM remains exact");
    }
    int bankCloseWaits = 0;
    juce::File realBankInput, realBankFile, realBankReference, realBankCorrupt;
    juce::Array<juce::File> realBankExtracted;
    juce::Array<juce::var> realBankChecks;
    SingleTrackDocument::WorkspaceSnapshot::Ptr realBankWorkspace, realBankCloseWorkspace;
    SingleTrackDocument::StatePtr realBankCloseEmptyState;
    media::OptionalMediaOperation::DiagnosticHandle realBankDiagnostic;
    std::weak_ptr<const void> realBankWorker;
    std::weak_ptr<const BnkDocument::State> realBankState;
    juce::Component::SafePointer<BnkAudioListViewComponent> realBankView;
    int64_t realBankFrames = 0;
    double realBankMaxError = 0.0;
    SingleTrackDocument::WorkspaceSnapshot::Ptr pendingCloseWorkspace;
    juce::AudioBuffer<float> pendingClosePcm { 2, 128 };
    juce::File parent, fixture, source, recoveryRoot, journal, artifacts, originalWorkingDirectory, verificationAsset, verificationHeld, videoSource;
    SingleTrackDocument::StatePtr packedEditedState;
    SingleTrackDocument::StatePtr bankScopePriorState;
    SingleTrackDocument::StatePtr videoOriginalState;
    juce::AudioBuffer<float> videoOriginalPcm { 2, 128 };
    bool videoMode = false, videoBudgetMode = false, videoStartupMode = false, packedMode = false, decoderChild = false;
    bool packedBudgetMode = false, packedStartupMode = false, packedCloseMode = false;
    bool packedRecoveredMode = false, packedEditedMode = false, packedBankMode = false;
    bool restoreSaveMode = false;
    bool compactedRestoreMode = false;
    SingleTrackDocument::StatePtr restoredSaveState;
    std::vector<float> restoredSaveHistoryPcm;
    juce::String bankScenario;
    juce::String trayScenario;
    juce::String hrtfScenario;
    bool hrtfStartupRendererPreparationObserved = false;
    bool hrtfStartupRendererPreparedAfterCore = false;
    SingleTrackDocument::StatePtr hrtfState;
    std::weak_ptr<const AmbisonicBinauralFilterBank> hrtfLifetime;
    bool diagnosticsMode = false, diagnosticHadDevice = false;
    bool deviceDiagnosticsMode = false, reproductionDiagnosticsMode = false;
    bool activeProbeMode = false, realProbeCompanion = false;
    bool guiDeviceProbeMode = false;
    bool spatialDiagnosticsMode = false;
    bool probeAdmissionLogged = false;
    double probeRetainedPosition = 0.0;
    std::weak_ptr<diagnostics::GuiDeviceProbe> probeLifetime;
    juce::Component::SafePointer<juce::DocumentWindow> diagnosticReopenWindow;
    bool diagnosticReopenChecked = false;
    std::vector<std::weak_ptr<const void>> diagnosticJobs, diagnosticSnapshots;
    bool diagnosticSawChooser = false;
    bool diagnosticStartupQuiet = false;
    diagnostics::testing::ActivitySnapshot diagnosticReleasedActivity {};
    int probePhase = 0, probeCancelMode = 0;
    juce::String probeDeviceState, probeBackend, probeOperation, probeFailure, probeWorkerWitness;
    juce::AudioDeviceManager::AudioDeviceSetup probeDeviceSetup;
    bool sessionDiagnosticsMode = false;
    juce::Uuid sessionDiagnosticFailure { nullptr };
    juce::String sessionDiagnosticPhase;
    bool mediaDiagnosticChoosing = false;
    bool bankScopeQueued = false;
    juce::String diagnosticOptionalScenario;
    bool diagnosticCacheWarmed = false;
    juce::File diagnosticSource;
    juce::String diagnosticFirstOperation;
    std::unique_ptr<SingleTrackDocument> diagnosticColdDocument;
    std::unique_ptr<AudioEngine> diagnosticColdEngine;
    juce::String diagnosticReview, bankDiagnosticReview;
    std::shared_ptr<const SingleTrackDocument::WorkspaceSnapshot> diagnosticWorkspace;
    juce::File queuedOpenBank, fallbackNewAudio;
    std::optional<SingleTrackDocument::CommittedStateSnapshot> fallbackHistory;
    std::optional<SingleTrackDocument::CommittedStateSnapshot> sourceRetryHistory;
    juce::String cleanupScenario;
    std::optional<media::DocumentIdentity> fallbackRecoveryOrigin;
    std::vector<juce::AudioBuffer<float>> fallbackRedoPcm;
    int bankChosenInput = 0;
    std::shared_ptr<const SingleTrackDocument::WorkspaceSnapshot> bankWorkspace;
    juce::Uuid packedReturnedEntry { nullptr };
    juce::File packedBankFile, packedBankCache;
    juce::String packedBankCacheHash;
    BnkDocument::StatePtr packedBankState;
    juce::Uuid firstOwner{ nullptr };
    juce::Uuid inspectionFailureOperation { nullptr };
    juce::Uuid restoreDiagnosticFailure { nullptr };
    int centerDiagnosticRejectedFolders = 0;
    juce::String sourceHash;
    std::shared_ptr<const SingleTrackDocument::WorkspaceSnapshot> centerImage;
    std::shared_ptr<juce::WaitableEvent> storageReadEntered, storageReadRelease;
    bool storageMode = false, storageChecked = false, capacityMode = false, retentionMode = false;
    juce::Component::SafePointer<juce::DocumentWindow> deferredCenter;
    juce::ApplicationProperties properties;
    std::unique_ptr<juce::LookAndFeel_V4> chrome, look;
    std::unique_ptr<SingleTrackDocument> document;
    std::unique_ptr<BnkDocument> bank;
    std::unique_ptr<PlaylistModel> playlist;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<WaveformModel> waveform;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<juce::FileOutputStream> quotaBlock;
    bool standalonePresentation = false;
    bool optionalLifecycleMode = false, optionalLifecycleCompleted = false;
    std::shared_ptr<OptionalLifecycleTestSession> optionalLifecycle;
    bool magLifecycleMode = false, magLifecycleCompleted = false, magTransitionsMode = false;
    std::shared_ptr<MagLifecycleTestSession> magLifecycle;
    juce::String backendLifecycleScenario;
    std::shared_ptr<BackendLifecycleTestSession> backendLifecycle;
    std::shared_ptr<DeviceBackendLifecycleTestSession> deviceBackendLifecycle;
    int stage = 0;
    double stageStarted = 0;
    bool failed = false, expectedQuit = false, invalidRoot = false, centerMode = false, emptyCenter = false, restoreMode = false, inspectionMode = false, earlierMode = false, inspectedCloseFailure = false;
};
}

START_JUCE_APPLICATION(singleeditor::NativeRecoveryTestApplication)
