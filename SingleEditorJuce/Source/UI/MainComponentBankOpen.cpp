#include "UI/MainComponent.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor
{
bool MainComponent::bnkOpenWorkspaceMatches(const BnkOpenIntent& intent) const
{
    const auto* entry = playlistModel.getCurrentEntry();
    if ((entry ? std::optional<juce::Uuid>(entry->id) : std::nullopt) != intent.expectedEntry
        || currentSelection != intent.previousSelection || bnkDocument.getState() != intent.previousBank
        || bnkDocument.getSelectedEventId() != intent.previousEvent || bnkDocument.getSelectedMediaId() != intent.previousMedia) return false;
    if (intent.previousWorkspace)
    {
        const auto current = document.captureWorkspaceSnapshot();
        return current && intent.previousWorkspace->sameContents(*current);
    }
    return intent.previousWasEmpty && document.getState() == intent.previousState && !document.getFileInfo().isValid();
}

bool MainComponent::restoreBnkOpenSelection(const BnkOpenIntent& intent)
{
    const auto* entry = playlistModel.getCurrentEntry();
    if (!intent.returnEntry || intent.token != pendingLoadToken.load(std::memory_order_relaxed)
        || (entry ? std::optional<juce::Uuid>(entry->id) : std::nullopt) != intent.expectedEntry
        || bnkDocument.getState() != intent.previousBank
        || (!intent.previousDocument.sameDocument(document.getMediaIdentity().document)
            && !(intent.previousWasEmpty && document.getState() == intent.previousState))) return false;
    // Returning the row belongs to the still-live document, even after edits
    // changed its history. Publishing the candidate uses the stricter full
    // workspace comparison above. Never override a newer document/selection.
    const auto index = playlistModel.indexOfEntry(*intent.returnEntry);
    if (index < 0) return false;
    const juce::ScopedValueSetter<bool> guard(suppressPlaylistLoad, true);
    playlistModel.setCurrentIndex(index);
    return true;
}

void MainComponent::beginBnkOpen(const juce::File& file, BnkOpenRequest::Capability capability,
    media::OptionalMediaFailureState::Intent action, std::optional<media::OptionalMediaFailureState::Key> reviewed)
{
    if (closeApplicationPending) return;
    if (!cancelOrdinaryAudioLoad()) return;
    cancelBnkPreview(); cancelPackedAudioLoad();
    if (pendingVideoOperation) pendingVideoOperation->requestCancel();
    if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    if (!optionalFailureSession) optionalFailureSession = std::make_shared<media::OptionalMediaFailureState>();
    BnkOpenIntent intent;
    intent.request = std::make_shared<BnkOpenRequest>(optionalFailureSession, file,
        workspaceSession.getMappedBnkProjectRoot(file), capability, action, std::move(reviewed));
    intent.previousState = document.getState(); intent.previousWorkspace = document.captureWorkspaceSnapshot();
    intent.previousDocument = document.getMediaIdentity().document;
    intent.previousBank = bnkDocument.getState(); intent.previousSelection = currentSelection;
    intent.previousEvent = bnkDocument.getSelectedEventId(); intent.previousMedia = bnkDocument.getSelectedMediaId();
    intent.previousWasEmpty = intent.previousState && !intent.previousState->hasPlaybackSource() && !document.getFileInfo().isValid();
    intent.preserveAudio = capability == BnkOpenRequest::Capability::sidecar && bnkDocument.getBankInfo().file == file;
    if (const auto* entry = playlistModel.getCurrentEntry()) intent.expectedEntry = entry->id;
    const auto previousFile = bnkDocument.getBankInfo().file != juce::File() ? bnkDocument.getBankInfo().file : document.getFileInfo().file;
    for (int i = 0; i < playlistModel.getNumEntries(); ++i)
    {
        const auto* entry = playlistModel.getEntry(i);
        if (!entry) continue;
        if (entry->recovered && workspaceSession.isActiveRecoveredDocument(entry->id)) { intent.returnEntry = entry->id; break; }
        if (!intent.returnEntry && !entry->recovered && entry->file == previousFile) intent.returnEntry = entry->id;
    }
    cancelCacheWarm(); loadThreadPool.removeAllJobs(false, 0);
    if (queuedBankOpen) queuedBankOpen->request->requestCancel();
    if (bankOpenLoad) bankOpenLoad->work->request->requestCancel();
    if (bankOpenProgressWindow) bankOpenProgressWindow->exitModalState(0);
    if (bankOpenFailureWindow) bankOpenFailureWindow->exitModalState(0);
    intent.token = pendingLoadToken.fetch_add(1, std::memory_order_relaxed) + 1;
    queuedBankOpen = std::move(intent); lastBnkOpenRequest = queuedBankOpen->request;
    lastBnkOpenDiagnostic = lastBnkOpenRequest->diagnosticHandle();
    bankOpenProgressToken = queuedBankOpen->token;
    pendingLoadLabel = file.getFileName(); loadInProgress = true;
    pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
    auto* progress = new juce::AlertWindow("Opening BNK", "Reading the bank and its optional metadata. Current audio and history are retained.",
        juce::MessageBoxIconType::NoIcon, this);
    progress->addButton("Cancel BNK open", 1, juce::KeyPress(juce::KeyPress::escapeKey));
    bankOpenProgressWindow = progress;
    progress->enterModalState(true, juce::ModalCallbackFunction::create([request = lastBnkOpenRequest](int choice)
        { if (choice == 1) request->requestCancel(); }), true);
    refreshUiState(); pollBnkOpen();
}

void MainComponent::cancelBnkOpen()
{
    const auto token = pendingLoadToken.load(std::memory_order_relaxed);
    const bool current = (queuedBankOpen && queuedBankOpen->token == token) || (bankOpenLoad && bankOpenLoad->intent.token == token);
    if (queuedBankOpen) { queuedBankOpen->request->requestCancel(); queuedBankOpen.reset(); }
    if (bankOpenLoad)
    {
        bankOpenLoad->work->request->requestCancel();
        if (bankOpenLoad->work->finished.load(std::memory_order_acquire))
        { bankOpenLoad->work->request->abandon(); bankOpenLoad.reset(); }
    }
    if (bankOpenProgressWindow) bankOpenProgressWindow->exitModalState(0);
    if (bankOpenFailureWindow) bankOpenFailureWindow->exitModalState(0);
    if (current) { loadInProgress = false; pendingLoadLabel.clear(); }
}

void MainComponent::pollBnkOpen()
{
    if (bankOpenLoad)
    {
        if (closeApplicationPending || bankOpenLoad->intent.token != pendingLoadToken.load(std::memory_order_relaxed)
            || !bnkOpenWorkspaceMatches(bankOpenLoad->intent)) bankOpenLoad->work->request->requestCancel();
        if (!bankOpenLoad->work->finished.load(std::memory_order_acquire)) return;
        auto load = std::move(*bankOpenLoad); bankOpenLoad.reset();
        const auto request = load.work->request;
        const bool current = load.intent.token == pendingLoadToken.load(std::memory_order_relaxed);
        const bool eligible = current && !closeApplicationPending && bnkOpenWorkspaceMatches(load.intent);
        if (bankOpenProgressWindow && bankOpenProgressToken == load.intent.token) bankOpenProgressWindow->exitModalState(0);
        if (current) { loadInProgress = false; pendingLoadLabel.clear(); }
        auto prepared = std::move(load.work->prepared);
        bool published = false;
        if (eligible && !request->wasCancelled() && prepared)
        {
            if (request->canPublish(prepared))
            {
                if (!load.intent.preserveAudio) workspaceSession.retainCurrentAudioDocumentIfNeeded();
                if (bnkDocument.adoptParsedState(prepared.state))
                {
                    published = true;
                    if (!load.intent.preserveAudio)
                    {
                        audioEngine.stop(); beginDocumentGeneration(request->file());
                        if (!workspaceSession.shouldRetainPlaylistAudio()) workspaceSession.clearRetainedPlaylistAudio();
                        workspaceSession.releaseRecoveredActivation(); document.clear();
                        currentSelection.reset(); waveformView.clearSelection(); waveformView.clearSelectionPreviewGain();
                    }
                    else if (load.intent.previousMedia) bnkDocument.selectMediaById(*load.intent.previousMedia);
                    else if (load.intent.previousEvent) bnkDocument.selectEventById(*load.intent.previousEvent);
                    request->succeed();
                    selectLoadedAudioEntry(request->file());
                    abandonActiveLoadTimingProbe("bnk-open");
                    ensureBnkUiComponents(); bnkCenterMode = prepared.state->events.empty() ? BnkCenterMode::audio : BnkCenterMode::events;
                    updateCenterViewVisibility(); notifyPrimaryContentVisibleIfNeeded();
                    refreshUiState(); repaintMetadataSidebarIfVisible(); repaint(); promoteDeferredChromeIfNeeded(180);
                }
                else request->failPublication();
            }
            else request->failPublication();
        }
        else if (!eligible || request->wasCancelled()) request->abandon();

        // Release both the candidate's bank lease and the snapshots before
        // continuing close or presenting an interactive failure/retry dialog.
        const bool sidecarUnavailable = published && !prepared.sidecar;
        prepared = {};
        const bool selectionRestored = !published && restoreBnkOpenSelection(load.intent);
        if (!published && (eligible || selectionRestored))
        {
            setDocumentOperationStatus("BNK open ended; current audio and complete history were kept.");
            refreshUiState();
        }
        load.intent.previousWorkspace.reset(); load.intent.previousState.reset(); load.intent.previousBank.reset();
        if (current && eligible && !request->wasCancelled())
        {
            if (!published) showBnkOpenFailure(request, BnkOpenRequest::Capability::core);
            else if (sidecarUnavailable) showBnkOpenFailure(request, BnkOpenRequest::Capability::sidecar);
        }
        if (closeApplicationPending) { continueCloseApplication(); return; }
    }
    if (!queuedBankOpen) return;
    auto intent = std::move(*queuedBankOpen); queuedBankOpen.reset();
    if (intent.request->wasCancelled() || closeApplicationPending || intent.token != pendingLoadToken.load(std::memory_order_relaxed)
        || !bnkOpenWorkspaceMatches(intent))
    {
        intent.request->requestCancel(); intent.request->abandon();
        restoreBnkOpenSelection(intent);
        if (bankOpenProgressWindow && bankOpenProgressToken == intent.token) bankOpenProgressWindow->exitModalState(0);
        if (intent.token == pendingLoadToken.load(std::memory_order_relaxed))
        { loadInProgress = false; pendingLoadLabel.clear(); refreshUiState(); }
        return;
    }
    auto work = std::make_shared<BnkOpenWork>(); work->request = intent.request;
    bankOpenLoad = BnkOpenLoad { std::move(intent), work };
    try
    {
        std::thread([work]
        {
#if JUCE_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            try { work->prepared = work->request->prepare(); }
            catch (...) { work->request->failPublication(); }
            work->finished.store(true, std::memory_order_release);
        }).detach();
    }
    catch (...) { work->request->failPublication(); work->finished.store(true, std::memory_order_release); }
}

void MainComponent::showBnkOpenFailure(const BnkOpenRequest::Ptr& request, BnkOpenRequest::Capability capability)
{
    using P = media::OptionalMediaFailureState;
    const bool metadata = capability == BnkOpenRequest::Capability::sidecar;
    const auto report = request->report(capability); const auto view = request->policyView(capability);
    juce::String message = metadata ? "Optional BNK metadata is unavailable. The proven bank core remains usable."
        : "The bank could not be opened. Current audio and complete Undo/Redo history were kept.";
    if (report.failure == BnkParser::Failure::deadline) message += "\nThe work exceeded its time budget.";
    else if (report.failure == BnkParser::Failure::hardLimit || report.failure == BnkParser::Failure::invalidData)
        message += "\nThe input is malformed or exceeds a fixed parser limit.";
    else if (report.failure == BnkParser::Failure::ioFailure) message += "\nAn input could not be read with its original identity.";
    else if (report.failure == BnkParser::Failure::memoryQuota) message += "\nThe work could not fit in memory.";
    if (view.circuitOpen) message += "\nTwo matching failures disabled this capability for this application session.";
    if (report.denied == P::Denied::invalidKey) message += "\nInputs changed since the reviewed failure. Open the bank again as a fresh request.";
    if (view.expansionAvailable) message += metadata ? "\nExpanded Budget permits up to 20 seconds of metadata parsing; fixed input limits remain."
        : "\nExpanded Budget permits up to 2 minutes of indexing; fixed input limits remain.";
    auto* alert = new juce::AlertWindow(metadata ? "BNK metadata unavailable" : "BNK open unavailable", message,
        juce::MessageBoxIconType::WarningIcon, this);
    if (view.retryAvailable && report.key) alert->addButton("Retry once", 1);
    if (view.expansionAvailable && report.key) alert->addButton("Retry with Expanded Budget", 2);
    alert->addButton("Export diagnostics...", 3);
    alert->addButton(metadata ? "Keep bank" : "Keep current audio", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    bankOpenFailureWindow = alert;
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<MainComponent>(this), request, capability, key = report.key](int choice)
        {
            if (!safe || safe->lastBnkOpenRequest != request) return;
            if (choice == 3) { safe->exportBnkOpenDiagnostics(request); return; }
            if ((choice == 1 || choice == 2) && key)
            {
                auto retry = [safe, request, capability, key, choice]
                { if (safe) safe->beginBnkOpen(request->file(), capability, choice == 1 ? P::Intent::retryOnce : P::Intent::expandBudget, key); };
                if (capability == BnkOpenRequest::Capability::core) safe->confirmDiscardOrSaveIfNeeded("retrying the BNK open", std::move(retry));
                else retry();
            }
        }), true);
}

void MainComponent::exportBnkOpenDiagnostics(const BnkOpenRequest::Ptr& request)
{
    if (!request || request != lastBnkOpenRequest) return;
    lastBnkOpenDiagnostic = request->diagnosticHandle();
    showLocalDiagnostics(std::set { diagnostics::ProviderKind::codec });
}
}
