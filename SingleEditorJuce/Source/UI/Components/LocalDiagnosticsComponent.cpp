#include "LocalDiagnosticsComponent.h"
#include "Diagnostics/ApplicationDiagnosticIdentity.h"
#include "Diagnostics/DiagnosticTestActivity.h"
#include "Diagnostics/ReproductionFiles.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include <juce_cryptography/juce_cryptography.h>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace singleeditor
{
namespace
{
using namespace diagnostics;
BundleIdentity applicationIdentity(const std::atomic<bool>& cancelled)
{
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    return captureApplicationIdentity(cancelled, "native-test");
#else
    return captureApplicationIdentity(cancelled, "local-build");
#endif
}
juce::String reviewText(const PreparedSnapshot& snapshot)
{
    const auto& b = snapshot.content();
    const bool probeReport = std::any_of(b.entries.begin(), b.entries.end(), [](const auto& entry)
    {
        if (entry.kind != EntryKind::provider) return false;
        const auto fields = juce::JSON::parse(entry.bytes.toString())["fields"];
        return (entry.id == "providers/codec.json" && fields["lastOperationModule"]["value"].toString() == "gui-codec-probe")
            || (entry.id == "providers/audio.json" && fields["negotiationRequestKind"]["value"].toString() == "gui-active-probe");
    });
    const auto& id = b.identity;
    juce::String text = "Snapshot: " + b.snapshotId.toString() + "\nCaptured: "
        + juce::Time(b.timestampMs).toISO8601(true) + "\nPrivacy: "
        + (b.tier == PrivacyTier::supportSafe ? "technical facts only" : b.tier == PrivacyTier::localIdentifiers
            ? "selected local identifiers included" : "selected reproduction contents included without redaction")
        + (snapshot.preview().attachmentCount == 0 ? "\nAudio and project contents: excluded"
            : "\nSelected reproduction files: included in full. Their contents are not opened in this preview.")
        + "\n\nApplication identity\nVersion: " + id.version
        + "\nSource: " + id.commit + "\nBinary SHA-256: " + id.binarySha256
        + "\nBuild: " + id.configuration + " / " + id.distribution
        + "\nArchitecture: " + id.processArchitecture + " / OS " + id.osArchitecture
        + "\nOS: " + id.osBuild + "\nTiming instrumentation: " + (id.timingInstrumentation ? "on" : "off")
        + "\nOrigin: " + b.origin;
    const auto includes = [&](const char* section)
    { return std::any_of(b.entries.begin(), b.entries.end(), [&](const auto& entry) { return entry.id == section; }); };
    text += "\n\nOnly checked, loaded sections are included below. Missing facts stay unreported."
            "\nCurrent state and a retained earlier failure are separate; use operation IDs to distinguish them.\n";
    if (includes("providers/audio.json"))
    {
        if (probeReport)
            text += "\nActive device probe: this is the explicit temporary operation, not the current playback state. "
                "Probe cleanup and restoration of the GUI's original device are separate outcomes. Refresh does not rerun it. "
                "Physical channel numbers shown to the user start at one; channelMapping uses zero-based indices.\n";
        text += "\nAudio: rates are Hz; buffer/latency are samples. Default selections have no requested numeric value."
                " Driver metadata source distinguishes the Windows adapter description and installed INF version from"
                " the initialized ASIO driver's reported name and raw integer version (driver-defined encoding)."
                " Neither identifies a loaded binary; unsupported vendor or hardware facts stay unavailable."
                " The speaker mask declares endpoint capability, not verified wiring."
                "\nAudio failure counters: requested Hz (0=default/unknown), buffer (0=default), outputs (-1=unspecified),"
                " default-channel flag, failed step, reserved. Codes are integration outcomes, not native driver codes.\n";
    }
    if (includes("providers/spatial.json"))
        text += "\nSpatial: monitor mode is a preference, not proof of an active output matrix.\n";
    if (includes("providers/codec.json"))
        text += (probeReport ? "\nActive codec probe: media facts describe the explicitly selected probe file, not the current Document. Refresh does not rerun this probe."
                             : "\nMedia facts describe the current document. Last-operation and bank-open facts may describe a failed request.")
                + juce::String(
                " Bank core and sidecar outcomes are separate; parser detail is a typed outcome."
                "\nMedia failure counters: exit code, observed bytes, allowed bytes, elapsed ms, cleanup confirmed, reserved."
                " Bank parser work does not use decoder counters.\n");
    if (includes("providers/cache.json"))
        text += "\nCache: the last operation's cache steps and totals, not the current document's recovery backing.\n";
    if (includes("providers/recovery.json"))
        text += "\nRecovery: current reasons/queues, the selected inspection and the last task failure are separate."
                " A successful retry retains the prior failure. Owner identity is a digest; journal sequence is not a Document version."
                "\nWriter counters: pending packets, retained bytes, published sequence, cancelled packets, reserved, reserved."
                " Maintenance counters: batches, removed files, removed bytes, frozen sequence, reserved, reserved."
                "\nInspection counters: metadata bytes, source window bytes, source full bytes, audio bytes, verified assets, selected sequence."
                " Source byte counters of -1 mean the failed check did not report them."
                " Inspection facts are from the last completed check of the selected point; unchecked audio is not verified."
                " No inspection or source read is started by this snapshot."
                "\nRestore counters: job count, resident bytes (-1=unreported), selected sequence, core-admitted flag, admission-wait flag, attempt-close error."
                " Worker launch IDs are separate from restore attempts; a queue launch has no selected owner or sequence."
                "\nSession counters: identity event sequence, retired epochs, current actor present, checkpoint required, closed, reserved."
                " Session IDs identify an observation lifetime; explicit checkpoint requests have distinct IDs."
                "\nHost counters: departed sessions, pending metadata, saved tickets, active actor present, closing, Save request serial."
                " Host IDs identify actual failed operations. Event sequences and Save serials are not Document versions."
                "\nService counters: waiting tickets, released epochs, completed owners, cancelled packets, removed files, removed bytes.\n";
    for (const auto& entry : b.entries)
    {
        text += "\n" + entry.id + "\n";
        if (entry.kind == EntryKind::reproduction)
            text += "Opaque attachment: " + juce::String(static_cast<juce::int64>(entry.bytes.getSize())) + " bytes\nSHA-256: "
                + juce::SHA256(entry.bytes).toHexString() + "\nContents are not previewed.\n";
        else text += juce::JSON::toString(juce::JSON::parse(entry.bytes.toString()), false) + "\n";
    }
    return text;
}
juce::String exportFailure(const BundleStatus& status)
{
    juce::String text;
    switch (status.error)
    {
        case BundleError::destinationExists: text = "That file already exists. Choose a new name."; break;
        case BundleError::cancelled: text = "Export cancelled."; break;
        case BundleError::budget: text = "The snapshot exceeds the diagnostic bundle limit."; break;
        case BundleError::ioFailure: text = "Could not save here. Check the folder and available space, then try again."; break;
        default: text = "The bundle could not be prepared or verified. Refresh the snapshot and try again."; break;
    }
    if (status.partialRetained) text += " An incomplete temporary file was retained in the selected folder.";
    return text;
}
}

struct LocalDiagnosticsComponent::Job final
{
    Job() { SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(job); }
    std::atomic<bool> cancelled { false }, complete { false };
    bool exporting = false, probing = false;
    std::optional<diagnostics::CodecProbeResult> probeResult;
    std::shared_ptr<diagnostics::GuiDeviceProbe> deviceProbe;
    std::shared_ptr<const diagnostics::PreparedSnapshot> snapshot;
    diagnostics::BundleStatus result { diagnostics::BundleError::invalidInput };
    juce::String message;
};

#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
LocalDiagnosticsComponent::ResourceObservation LocalDiagnosticsComponent::resourcesForTesting() const
{
    return {job, prepared, isTimerRunning(), chooser != nullptr};
}
#endif

LocalDiagnosticsComponent::LocalDiagnosticsComponent(Capture source, diagnostics::ProviderScope selected, std::vector<juce::File> excluded,
                                                     std::function<bool()> canProbe, DiagnosticDeviceActions device)
    : capture(std::move(source)), probeAllowed(std::move(canProbe)), deviceActions(std::move(device)),
      scope(std::move(selected)), exportExcludedDirectories(std::move(excluded))
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(component);
    setName("Local diagnostics");
    heading.setText("Review a snapshot, then save it locally. Nothing is uploaded.", juce::dontSendNotification);
    heading.setFont(juce::Font(juce::FontOptions(17.0f)));
    for (auto [kind, button] : sectionButtons)
    {
        addAndMakeVisible(button);
        button->setName("Diagnostic section " + button->getButtonText());
        button->onClick = [this]
        {
            scope = std::set<diagnostics::ProviderKind>{};
            for (auto [provider, control] : sectionButtons) if (control->getToggleState()) scope->insert(provider);
            refresh();
        };
    }
    for (auto* item : { &includePath, &includeName, &includeDevice })
    {
        addAndMakeVisible(item);
        item->onClick = [this] { refresh(); };
    }
    preview.setName("Diagnostic snapshot preview");
    preview.setMultiLine(true); preview.setReadOnly(true); preview.setScrollbarsShown(true);
    preview.setFont(juce::Font(juce::FontOptions(13.0f)));
    status.setName("Diagnostic status");
    status.setMinimumHorizontalScale(1.0f);
    reproductionStatus.setName("Diagnostic reproduction selection");
    reproductionStatus.setText("No reproduction files selected", juce::dontSendNotification);
    for (juce::Component* item : std::array<juce::Component*, 12> { &heading, &preview, &status, &refreshButton, &saveButton,
        &reproductionButton, &removeReproductionButton, &reproductionStatus, &probeButton, &deviceProbeButton, &abortProbeButton, &currentStateButton })
        addAndMakeVisible(item);
    refreshButton.onClick = [this] { refresh(); };
    saveButton.onClick = [this] { chooseDestination(); };
    reproductionButton.onClick = [this] { chooseReproduction(); };
    probeButton.onClick = [this] { chooseProbe(); };
    deviceProbeButton.onClick = [this] { chooseDeviceProbe(); };
    abortProbeButton.onClick = [this]
    {
        if (job && job->probing)
        {
            job->cancelled.store(true);
            if (job->deviceProbe) job->deviceProbe->cancel();
            abortProbeButton.setEnabled(false); status.setText("Aborting the probe; device restoration may still be pending.", juce::dontSendNotification);
        }
    };
    currentStateButton.onClick = [this]
    { probeObservation.reset(); scope.reset(); requestRefresh(); };
    removeReproductionButton.onClick = [this]
    {
        selectedReproductionFiles.clear();
        reproductionStatus.setText("No reproduction files selected", juce::dontSendNotification);
        requestRefresh();
    };
    setSize(780, 660);
    refresh();
}
LocalDiagnosticsComponent::~LocalDiagnosticsComponent()
{
    shuttingDown = true;
    reproductionConfirmation.close();
    probeConfirmation.close();
    stopTimer();
    if (job)
    {
        job->cancelled.store(true);
        if (job->deviceProbe) job->deviceProbe->cancel();
    }
    // Detached jobs own only copied evidence and output state, never this UI
    // or an engine/session. No worker completion callback can self-join.
}
void LocalDiagnosticsComponent::paint(juce::Graphics& g)
{
    g.fillAll(ModernAppleLookAndFeel::windowBackground());
}
void LocalDiagnosticsComponent::resized()
{
    auto area = getLocalBounds().reduced(20);
    heading.setBounds(area.removeFromTop(40));
    auto sections = area.removeFromTop(32);
    for (auto [kind, button] : sectionButtons) button->setBounds(sections.removeFromLeft(140));
    auto choices = area.removeFromTop(34);
    includePath.setBounds(choices.removeFromLeft(220)); includeName.setBounds(choices.removeFromLeft(220)); includeDevice.setBounds(choices);
    auto reproduction = area.removeFromTop(34);
    reproductionButton.setBounds(reproduction.removeFromLeft(240)); removeReproductionButton.setBounds(reproduction.removeFromLeft(120));
    reproductionStatus.setBounds(reproduction.reduced(8, 0));
    auto probes = area.removeFromTop(34);
    probeButton.setBounds(probes.removeFromLeft(160)); deviceProbeButton.setBounds(probes.removeFromLeft(160));
    abortProbeButton.setBounds(probes.removeFromLeft(120));
    currentStateButton.setBounds(probes.removeFromLeft(160).reduced(8, 0));
    area.removeFromTop(8);
    auto actions = area.removeFromBottom(34);
    refreshButton.setBounds(actions.removeFromLeft(160)); saveButton.setBounds(actions.removeFromRight(230));
    area.removeFromBottom(8); status.setBounds(area.removeFromBottom(48));
    preview.setBounds(area);
}
void LocalDiagnosticsComponent::busy(bool value)
{
    refreshButton.setEnabled(!value); saveButton.setEnabled(!value && prepared != nullptr);
    includePath.setEnabled(!value); includeName.setEnabled(!value); includeDevice.setEnabled(!value);
    reproductionButton.setEnabled(!value); removeReproductionButton.setEnabled(!value && !selectedReproductionFiles.empty());
    probeButton.setEnabled(!value && static_cast<bool>(probeAllowed) && availableProviders.contains(diagnostics::ProviderKind::codec));
    deviceProbeButton.setEnabled(!value && static_cast<bool>(deviceActions.current) && static_cast<bool>(deviceActions.start));
    abortProbeButton.setEnabled(job && job->probing && !job->cancelled.load());
    currentStateButton.setEnabled(!value && probeObservation.has_value());
    for (auto [kind, button] : sectionButtons) button->setEnabled(!value && availableProviders.contains(kind));
}
void LocalDiagnosticsComponent::refresh()
{
    if (job || chooser || confirmationPending || shuttingDown) return;
    if (queuedScope) { scope = std::move(*queuedScope); queuedScope.reset(); }
    refreshQueued = false;
    if (currentStateQueued) { probeObservation.reset(); currentStateQueued = false; }
    heading.setText(probeObservation ? "Active probe report. Refresh does not rerun the operation."
        : "Review a snapshot, then save it locally. Nothing is uploaded.", juce::dontSendNotification);
    prepared.reset(); preview.clear(); busy(true);
    status.setText("Collecting this snapshot...", juce::dontSendNotification);
    diagnostics::SnapshotSelection selection;
    if (includePath.getToggleState()) selection.localIdentifiers.insert(diagnostics::LocalIdentifierKind::path);
    if (includeName.getToggleState()) selection.localIdentifiers.insert(diagnostics::LocalIdentifierKind::fileName);
    if (includeDevice.getToggleState()) selection.localIdentifiers.insert(diagnostics::LocalIdentifierKind::deviceName);
    job = std::make_shared<Job>();
    auto state = job;
    try
    {
        std::vector<diagnostics::CapturedProvider> evidence;
        if (probeObservation)
        {
            evidence.push_back(*probeObservation);
        }
        else evidence = capture(); // Live owners are read on the message thread exactly once.
        availableProviders.clear();
        for (const auto& item : evidence) availableProviders.insert(item.kind);
        for (auto [kind, button] : sectionButtons)
        {
            // This JUCE Button ignores setToggleState while disabled. Sync on
            // this owner-thread turn, then keep controls busy until completion.
            button->setEnabled(true);
            button->setToggleState(availableProviders.contains(kind) && (!scope || scope->contains(kind)), juce::dontSendNotification);
            button->setEnabled(false);
        }
        if (scope) std::erase_if(evidence, [this](const auto& item) { return !scope->contains(item.kind); });
        // Excluded sections never reach worker enrichment, OS device metadata
        // reads or the prepared/exported snapshot. No absent owner is created.
        if (!juce::Thread::launch([state, evidence = std::move(evidence), selection, chosen = selectedReproductionFiles]() mutable
        {
            SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(worker);
            try
            {
                if (!chosen.empty())
                {
                    auto files = diagnostics::ReproductionFiles::capture(chosen, state->cancelled);
                    if (!files.status)
                    {
                        state->result = files.status;
                        state->message = files.status.error == diagnostics::BundleError::budget
                            ? "Selected files exceed the limit: 16 MiB each, 62 MiB total, at most "
                                + juce::String(static_cast<int>(diagnostics::ReproductionFiles::maximumFiles)) + " files. Remove or reselect them."
                            : files.status.error == diagnostics::BundleError::cancelled ? "Reproduction capture cancelled."
                            : "Could not read all selected files. Remove or reselect them and try again.";
                        state->complete.store(true, std::memory_order_release); return;
                    }
                    selection.reproductionConsent = true; selection.chosenReproduction = std::move(files.entries);
                }
                const auto identity = applicationIdentity(state->cancelled);
                diagnostics::enrichNativeDeviceMetadata(evidence, state->cancelled);
                std::vector<diagnostics::LoadedProviderRef> refs;
                for (const auto& item : evidence) refs.push_back({ item.kind, &item });
                auto snapshot = std::make_shared<diagnostics::PreparedSnapshot>(
                    diagnostics::DiagnosticSnapshot::capture(identity, "gui", refs, selection));
                state->result = snapshot->status();
                if (state->result && !state->cancelled.load()) state->snapshot = std::move(snapshot);
            }
            catch (...) { state->result.error = diagnostics::BundleError::ioFailure; }
            state->complete.store(true, std::memory_order_release);
        })) { state->result.error = diagnostics::BundleError::ioFailure; state->complete.store(true); }
    }
    catch (...) { state->result.error = diagnostics::BundleError::ioFailure; state->complete.store(true); }
    startTimer(40);
}
void LocalDiagnosticsComponent::chooseDestination()
{
    if (!prepared || job || chooser || confirmationPending || shuttingDown) return;
    const auto reviewed = prepared; // Freeze exactly what the user has reviewed.
    busy(true);
    chooser = std::make_unique<juce::FileChooser>("Save reviewed diagnostic bundle",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("SingleEditor-diagnostics-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".zip"), "*.zip");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<LocalDiagnosticsComponent>(this), reviewed](const juce::FileChooser& picker)
        {
            if (!safe) return;
            const auto destination = picker.getResult(); safe->chooser.reset();
            if (destination == juce::File())
            { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); return; }
            safe->publish(destination, reviewed);
        });
}
void LocalDiagnosticsComponent::chooseReproduction()
{
    if (job || chooser || confirmationPending || shuttingDown) return;
    busy(true);
    chooser = std::make_unique<juce::FileChooser>("Choose minimal reproduction audio or configuration files",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
        | juce::FileBrowserComponent::canSelectMultipleItems,
        [safe = juce::Component::SafePointer<LocalDiagnosticsComponent>(this)](const juce::FileChooser& picker)
        {
            if (!safe || safe->shuttingDown) return;
            const auto results = picker.getResults(); safe->chooser.reset();
            if (results.isEmpty())
            { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); return; }
            std::vector<juce::File> chosen(results.begin(), results.end());
            if (chosen.size() > diagnostics::ReproductionFiles::maximumFiles)
            {
                safe->busy(false); safe->status.setText("Choose at most "
                    + juce::String(static_cast<int>(diagnostics::ReproductionFiles::maximumFiles))
                    + " reproduction files. The reviewed snapshot is unchanged.", juce::dontSendNotification);
                if (safe->refreshQueued) safe->requestRefresh(); return;
            }
            juce::String paths; for (const auto& file : chosen) paths += file.getFullPathName() + "\n";
            safe->confirmationPending = true;
            safe->reproductionConfirmation = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
                .withTitle("Include reproduction contents?")
                .withMessage("These complete files will be copied into the local diagnostic bundle without redaction. Choose only minimal examples. Nothing is uploaded.\n\n"
                    + paths + "\nLimits: 16 MiB per file, 62 MiB total. Review the new snapshot before saving it.")
                .withButton("Include files").withButton("Cancel").withAssociatedComponent(safe.getComponent()),
                [safe, chosen = std::move(chosen)](int result) mutable
                {
                    if (!safe || safe->shuttingDown || !safe->confirmationPending) return;
                    safe->confirmationPending = false;
                    if (result == 1)
                    {
                        safe->selectedReproductionFiles = std::move(chosen);
                        safe->reproductionStatus.setText(juce::String(static_cast<int>(safe->selectedReproductionFiles.size()))
                            + " files selected; full contents", juce::dontSendNotification);
                        safe->busy(false); safe->requestRefresh();
                    }
                    else { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); }
                });
        });
}
void LocalDiagnosticsComponent::chooseProbe()
{
    if (job || chooser || confirmationPending || shuttingDown || !probeAllowed) return;
    if (!probeAllowed()) { status.setText("Stop playback or speaker testing before starting an active probe.", juce::dontSendNotification); return; }
    const auto worker = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SingleEditorCLI.exe");
    if (!worker.existsAsFile()) { status.setText("Audio probing is unavailable in this installation.", juce::dontSendNotification); return; }
    busy(true);
    chooser = std::make_unique<juce::FileChooser>("Choose audio for an active codec probe",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<LocalDiagnosticsComponent>(this)](const juce::FileChooser& picker)
        {
            if (!safe || safe->shuttingDown) return;
            const auto selected = picker.getResult(); safe->chooser.reset();
            if (selected == juce::File()) { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); return; }
            safe->confirmationPending = true;
            safe->probeConfirmation = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
                .withTitle("Run active audio probe?")
                .withMessage("Read this file in a temporary process, up to 8 MiB and 4096 initial frames, with a 5-second worker limit. "
                    "Native audio readers only; no external decoder fallback. No sound, device changes, cache repair or Document replacement. "
                    "This does not attach audio contents to the report. Abort is available while running.\n\n" + selected.getFullPathName())
                .withButton("Run probe").withButton("Cancel").withAssociatedComponent(safe.getComponent()),
                [safe, selected](int result)
                {
                    if (!safe || safe->shuttingDown || !safe->confirmationPending) return;
                    safe->confirmationPending = false;
                    if (result == 1) safe->startProbe(selected);
                    else { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); }
                });
        });
}
void LocalDiagnosticsComponent::chooseDeviceProbe()
{
    if (job || chooser || confirmationPending || shuttingDown || !deviceActions.current || !deviceActions.start) return;
    const auto selected = deviceActions.current();
    if (!selected)
    {
        status.setText("Device probe needs an open, idle output. Stop playback or speaker testing; select an output in Preferences if needed.", juce::dontSendNotification);
        return;
    }
    const auto& setup = selected->setup;
    if (setup.outputChannels.isZero() || setup.outputChannels.getHighestBit() >= 32)
    { status.setText("Device probe supports up to 32 physical outputs.", juce::dontSendNotification); return; }
    busy(true); confirmationPending = true;
    deviceOptions = std::make_unique<juce::AlertWindow>("Device probe options",
        selected->backend + "\n" + setup.outputDeviceName
        + "\nTemporarily release this output, probe it, then restore its exact original configuration. The Document and routing remain unchanged.",
        juce::MessageBoxIconType::InfoIcon, this);
    deviceOptions->addTextEditor("rate", juce::String(static_cast<int>(setup.sampleRate)), "Requested sample rate (0 = backend default)");
    deviceOptions->addTextEditor("buffer", juce::String(setup.bufferSize), "Requested buffer samples (0 = backend default)");
    juce::StringArray choices { "Silent negotiation" };
    std::vector<int> physicalChannels;
    for (int i = 0; i <= setup.outputChannels.getHighestBit(); ++i)
        if (setup.outputChannels[i]) { physicalChannels.push_back(i + 1); choices.add("Ping physical output " + juce::String(i + 1)); }
    deviceOptions->addComboBox("ping", choices, "Sound");
    deviceOptions->addButton("Review effects", 1); deviceOptions->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    deviceOptions->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<LocalDiagnosticsComponent>(this), selected = *selected, physicalChannels](int choice)
    {
        if (!safe || safe->shuttingDown || !safe->deviceOptions) return;
        const auto rate = safe->deviceOptions->getTextEditorContents("rate").trim();
        const auto buffer = safe->deviceOptions->getTextEditorContents("buffer").trim();
        const auto ping = safe->deviceOptions->getComboBoxComponent("ping")->getSelectedItemIndex();
        safe->deviceOptions.reset(); safe->confirmationPending = false;
        if (choice != 1) { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); return; }
        diagnostics::DeviceProbeRequest request;
        request.backend = selected.backend; request.outputDevice = selected.setup.outputDeviceName;
        request.expectedOutputEndpoint = selected.endpoint;
        request.sampleRate = rate.getIntValue(); request.bufferSamples = buffer.getIntValue();
        request.physicalOutputs = selected.setup.outputChannels;
        request.outputChannels = request.physicalOutputs.countNumberOfSetBits();
        if (ping > 0 && static_cast<size_t>(ping) <= physicalChannels.size()) request.pingChannel = physicalChannels[static_cast<size_t>(ping - 1)];
        if (rate.isEmpty() || buffer.isEmpty() || rate.length() > 6 || buffer.length() > 6
            || !rate.containsOnly("0123456789") || !buffer.containsOnly("0123456789") || !diagnostics::DeviceDiagnosticProbe::valid(request))
        {
            safe->busy(false); safe->status.setText("Invalid request: rate 0 or 8000–768000 Hz; buffer 0–65536 samples. No device work started.", juce::dontSendNotification);
            return;
        }
        safe->confirmationPending = true;
        const auto sound = request.pingChannel == 0 ? juce::String("Silent negotiation; no sound.")
            : "Sound on physical output " + juce::String(request.pingChannel) + ": 100 Hz, -40 dBFS peak, 250 ms with ramps.";
        safe->probeConfirmation = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
            .withTitle("Run active device probe?")
            .withMessage(request.backend + "\n" + request.outputDevice + "\nRequested rate: " + rate + " Hz; buffer: " + buffer
                + " samples (0 = backend default). Enabled physical mask (hex): " + request.physicalOutputs.toString(16)
                + "\n" + sound + "\n\nThe current device will close temporarily. A separate worker is limited to 5 seconds and 512 MiB. "
                    "Playback and device changes are unavailable until the original output is restored. Abort cancels the probe, then attempts restoration. "
                    "A driver that fails to return may leave restoration pending. Nothing is uploaded.")
            .withButton("Run device probe").withButton("Cancel").withAssociatedComponent(safe.getComponent()),
            [safe, selected, request](int result)
            {
                if (!safe || safe->shuttingDown || !safe->confirmationPending) return;
                safe->confirmationPending = false;
                if (result == 1) safe->startDeviceProbe(selected, request);
                else { safe->busy(false); if (safe->refreshQueued) safe->requestRefresh(); }
            });
    }), false);
}
void LocalDiagnosticsComponent::startDeviceProbe(const AudioEngine::DeviceProbeConfiguration& selected,
                                                const diagnostics::DeviceProbeRequest& request)
{
    if (job || shuttingDown) return;
    auto operation = deviceActions.start ? deviceActions.start(selected, request) : nullptr;
    if (!operation)
    {
        busy(false); status.setText("Device probe refused: the selected output changed or is busy. Review it again. No probe started.", juce::dontSendNotification);
        return;
    }
    job = std::make_shared<Job>(); job->probing = true; job->deviceProbe = std::move(operation);
    busy(true); status.setText("Releasing the selected output for the device probe...", juce::dontSendNotification);
    startTimer(40);
}
void LocalDiagnosticsComponent::startProbe(const juce::File& source)
{
    if (job || shuttingDown) return;
    if (!probeAllowed || !probeAllowed())
    {
        status.setText("Probe refused: playback or speaker testing is active. The current snapshot is unchanged.", juce::dontSendNotification);
        busy(false); if (refreshQueued) requestRefresh(); return;
    }
    job = std::make_shared<Job>(); job->probing = true;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // Resolve the document generation at this confirmed admission, not when
    // the diagnostics window was opened. The worker keeps that value.
    const auto observedContext = optionalLifecycleContextProvider
        ? optionalLifecycleContextProvider() : OptionalLifecycleContext{};
#endif
    const auto state = job;
    const auto worker = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SingleEditorCLI.exe");
    busy(true); status.setText("Running the selected audio probe. Abort is available.", juce::dontSendNotification);
    if (!juce::Thread::launch([state, source, worker, operation = juce::Uuid()
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , observedContext
#endif
    ]
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const ScopedOptionalLifecycleContext operationContext(observedContext);
#endif
        SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(worker);
        try { state->probeResult = diagnostics::CodecDiagnosticProbe::run(worker, source, operation, state->cancelled,
            diagnostics::CodecDiagnosticProbe::limits(), diagnostics::CodecDiagnosticProbe::Origin::gui); }
        catch (...) { state->result.error = diagnostics::BundleError::ioFailure; }
        state->complete.store(true, std::memory_order_release);
    })) { state->result.error = diagnostics::BundleError::ioFailure; state->complete.store(true); }
    startTimer(40);
}
void LocalDiagnosticsComponent::publish(const juce::File& destination, std::shared_ptr<const diagnostics::PreparedSnapshot> reviewed)
{
    // Preserve Recovery Center's existing namespace exclusion when using the
    // common writer. This is a lexical destination check, with no storage read.
    if (std::any_of(exportExcludedDirectories.begin(), exportExcludedDirectories.end(), [&](const auto& root)
        { return root != juce::File() && (destination == root || destination.isAChildOf(root)); }))
    {
        busy(false);
        status.setText("Choose a .zip output outside recovery storage. The reviewed snapshot is unchanged.", juce::dontSendNotification);
        if (refreshQueued) requestRefresh();
        return;
    }
    job = std::make_shared<Job>(); job->exporting = true;
    status.setText("Saving the reviewed snapshot...", juce::dontSendNotification);
    const auto state = job;
    if (!juce::Thread::launch([state, destination, reviewed]
    {
        SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(worker);
        try
        {
            state->result = diagnostics::DiagnosticSnapshot::publishReviewed(*reviewed, reviewed->preview().snapshotId, destination, &state->cancelled);
            if (state->result)
            {
                const auto checked = diagnostics::DiagnosticBundle::validate(destination, &state->cancelled);
                if (checked.status) state->message = "Saved and verified locally: " + destination.getFullPathName();
                else state->message = "Saved locally, but read-back verification did not complete. File retained: " + destination.getFullPathName();
            }
        }
        catch (...) { state->result.error = diagnostics::BundleError::ioFailure; }
        state->complete.store(true, std::memory_order_release);
    })) { state->result.error = diagnostics::BundleError::ioFailure; state->complete.store(true); }
    startTimer(40);
}
void LocalDiagnosticsComponent::timerCallback()
{
    if (job && job->deviceProbe)
    {
        const auto* result = job->deviceProbe->result();
        if (!result)
        {
            const auto phase = job->deviceProbe->phase();
            status.setText(job->deviceProbe->driverUnresolved()
                ? "The driver has not returned. Device restoration remains unresolved; this window can close."
                : phase == diagnostics::GuiDeviceProbe::Phase::restoring || phase == diagnostics::GuiDeviceProbe::Phase::awaitingReturn
                    ? "Restoring the original device configuration..."
                    : job->deviceProbe->cancellationRequested() ? "Cancelling the probe; restoration will follow."
                    : "Running the device probe. Playback and device setup are temporarily unavailable.", juce::dontSendNotification);
            return;
        }
        stopTimer();
        auto observed = result->evidence;
        const auto cancelled = job->cancelled.load() || result->cancelled;
        job.reset();
        const auto restoreFailed = std::any_of(observed.fields.begin(), observed.fields.end(), [](const auto& item)
            { return item.field == diagnostics::Field::deviceProbeRestore && item.value.toString() != "exact-device-restored"; });
        if (cancelled && !restoreFailed)
        {
            busy(false); status.setText("Probe cancelled; original device restored. Previous snapshot unchanged.", juce::dontSendNotification);
            if (refreshQueued) requestRefresh(); return;
        }
        if (observed.latestFailure) latestDeviceProbeFailure = observed.latestFailure;
        else if (latestDeviceProbeFailure) { observed.latestFailure = latestDeviceProbeFailure; observed.operationIds.push_back(latestDeviceProbeFailure->operationId); }
        diagnostics::CapturedProvider report; report.kind = diagnostics::ProviderKind::audio; report.evidence = std::move(observed);
        probeObservation = std::move(report); scope = std::set<diagnostics::ProviderKind>{ diagnostics::ProviderKind::audio };
        requestRefresh(); return;
    }
    if (job && job->probing && (!probeAllowed || !probeAllowed()))
    { job->cancelled.store(true); abortProbeButton.setEnabled(false); status.setText("Audio activity started; aborting the probe.", juce::dontSendNotification); }
    if (!job || !job->complete.load(std::memory_order_acquire)) return;
    stopTimer(); const auto finished = std::move(job);
    if (finished->probing)
    {
        if (finished->cancelled.load() || (finished->probeResult && finished->probeResult->cancelled))
            status.setText("Probe cancelled. The previous reviewed snapshot is unchanged.", juce::dontSendNotification);
        else if (finished->probeResult)
        {
            auto observed = std::move(finished->probeResult->evidence);
            if (observed.latestFailure) latestProbeFailure = observed.latestFailure;
            else if (latestProbeFailure) { observed.latestFailure = latestProbeFailure; observed.operationIds.push_back(latestProbeFailure->operationId); }
            diagnostics::CapturedProvider report; report.kind = diagnostics::ProviderKind::codec; report.evidence = std::move(observed);
            probeObservation = std::move(report);
            scope = std::set<diagnostics::ProviderKind>{ diagnostics::ProviderKind::codec };
            requestRefresh(); return;
        }
        else status.setText("Could not start or finish the selected probe. The previous snapshot is unchanged.", juce::dontSendNotification);
    }
    else if (finished->exporting)
        status.setText(finished->result ? finished->message : exportFailure(finished->result), juce::dontSendNotification);
    else if (finished->snapshot)
    {
        prepared = finished->snapshot; preview.setText(reviewText(*prepared), false);
        const auto restoreFailed = probeObservation && std::any_of(probeObservation->evidence.fields.begin(),
            probeObservation->evidence.fields.end(), [](const auto& item)
            { return item.field == diagnostics::Field::deviceProbeRestore && item.value.toString() != "exact-device-restored"; });
        status.setText(restoreFailed
            ? "Device restoration failed. Playback is unavailable; explicitly select an output in Preferences. This report is ready to review."
            : "Ready to review. " + juce::String(static_cast<int>(prepared->preview().providerCount))
            + " providers captured. Refresh explicitly to update this snapshot.", juce::dontSendNotification);
    }
    else status.setText(finished->message.isNotEmpty() ? finished->message : exportFailure(finished->result), juce::dontSendNotification);
    busy(false);
    if (refreshQueued) requestRefresh();
}
void LocalDiagnosticsComponent::requestRefresh()
{
    if (job || chooser || confirmationPending) { refreshQueued = true; return; }
    refresh();
}
void LocalDiagnosticsComponent::requestScope(diagnostics::ProviderScope selected)
{
    currentStateQueued = true;
    queuedScope.emplace(std::move(selected));
    requestRefresh();
}
}
