#pragma once
#include "DiagnosticSnapshot.h"
#include "Audio/OptionalMediaOperation.h"
#include "Bnk/BnkOpenRequest.h"
#include "Recovery/RecoveryInspection.h"
#include "Recovery/RecoveryRestoreQueue.h"

namespace singleeditor
{
class AudioEngine;
class SingleTrackDocument;
namespace recovery { class RecoveryService; }
namespace diagnostics
{
// One explicit owner-thread read. Only copied facts cross to the bundle worker.
struct CapturedProvider final : LoadedDiagnosticProvider
{
    ProviderKind kind = ProviderKind::audio;
    ProviderEvidence evidence;
    juce::String outputEndpoint; // copied owner binding, consumed only by explicit worker enrichment
    ProviderEvidence snapshot() const override { return evidence; }
};
void enrichNativeDeviceMetadata(std::vector<CapturedProvider>&, const std::atomic<bool>& cancelled);
std::vector<CapturedProvider> captureLiveDiagnosticProviders(const AudioEngine&, const SingleTrackDocument&,
                                                            const recovery::RecoveryService*,
                                                            const media::OptionalMediaOperation::DiagnosticHandle& = {},
                                                            const BnkOpenRequest::DiagnosticHandle& = {},
                                                            const media::OptionalMediaFailureState* = nullptr,
                                                            const recovery::RecoveryInspection::DiagnosticObservation* = nullptr,
                                                            const recovery::RecoveryDiagnosticState* = nullptr,
                                                            const recovery::RecoveryRestoreQueue* = nullptr,
                                                            const juce::Uuid& selectedRestore = juce::Uuid(nullptr));
}
}
