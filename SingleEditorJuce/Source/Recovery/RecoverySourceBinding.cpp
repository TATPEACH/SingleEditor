#include "RecoverySourceBinding.h"

namespace singleeditor::recovery
{
Status RecoverySourceBinding::resolve(const media::SourceIdentity& original,
    const std::optional<RecoverySourceEvidence>& evidence, const Request& request,
    std::unique_ptr<RecoverySourceBinding>& output, const std::atomic<bool>* cancelled)
{
    try
    {
        if (cancelled && cancelled->load(std::memory_order_acquire)) return { Error::cancelled, "source_resolution_cancelled" };
        if (original.binding.isNull() || (evidence && !evidence->bindsTo(original))
            || (request.verification != RecoverySourceIdentity::Mode::sparseAfterMutation
                && request.verification != RecoverySourceIdentity::Mode::explicitDeepVerify))
            return { Error::invalidPacket, "source_resolution_binding" };
        auto candidate = std::unique_ptr<RecoverySourceBinding>(new RecoverySourceBinding());
        candidate->original = original;
        candidate->relocationConfirmed = request.relocationConfirmed;
        const auto file = request.locatedFile == juce::File() ? original.location.file : request.locatedFile;
        if (file == juce::File())
        {
            candidate->resolution = { SourceConfidence::ambiguous, SourceReason::insufficientEvidence, {} };
            candidate->observation.location = file;
        }
        else
        {
            const auto before = RecoverySourceIdentity::observe(file, cancelled);
            if (!before.status) return before.status;
            if (request.expectedObservation && before.observation != *request.expectedObservation)
                return { Error::sourceConflict, "source_review_obsolete" };
            candidate->observation = before.observation;
            if (!before.observation.exists)
                candidate->resolution = { SourceConfidence::missing, SourceReason::absent, {} };
            else if (!evidence)
                candidate->resolution = { SourceConfidence::ambiguous, SourceReason::insufficientEvidence, {} };
            else if (request.verification != RecoverySourceIdentity::Mode::explicitDeepVerify
                && (evidence->sizeBytes != before.observation.sizeBytes || evidence->modifiedTicks != before.observation.modifiedTicks))
                candidate->resolution = { SourceConfidence::changed, SourceReason::metadataChanged, {} };
            else if (request.verification != RecoverySourceIdentity::Mode::explicitDeepVerify
                && evidence->fileId.isNotEmpty() && before.observation.fileId.isNotEmpty()
                && (evidence->fileId != before.observation.fileId || evidence->volumeId != before.observation.volumeId))
                candidate->resolution = { SourceConfidence::changed, SourceReason::fileIdentityChanged, {} };
            else
            {
                auto current = original; current.location.file = file;
                current.sizeBytes = before.observation.sizeBytes;
                current.modifiedMilliseconds = before.observation.modifiedTicks / 10000 - 11644473600000LL;
                current.fastStampValid = true;
                const auto coverage = evidence->algorithm == "source-file-3x64k-sha256-v1"
                    ? RecoverySourceIdentity::Coverage::encodedFile : RecoverySourceIdentity::Coverage::audioPayload;
                const auto probe = RecoverySourceIdentity::capture(current, evidence->facts, request.verification, cancelled, coverage);
                if (!probe.status)
                {
                    if (cancelled && cancelled->load(std::memory_order_acquire)) return { Error::cancelled, "source_resolution_cancelled" };
                    return probe.status.error == Error::staleHead ? Status { Error::sourceConflict, probe.status.code } : probe.status;
                }
                candidate->sparseBytes = probe.payloadBytesRead; candidate->deepBytes = probe.fullBytesRead;
                if (probe.evidence) candidate->deepHash = probe.evidence->fullFileSha256;
                candidate->resolution = RecoverySourceIdentity::compare(*evidence, probe.evidence);
            }
            // Covers a replacement during the probe or a decision on a changed/
            // missing source. Busy/unreadable is a fault, never "missing".
            const auto after = RecoverySourceIdentity::observe(file, cancelled);
            if (!after.status) return after.status;
            if (after.observation != before.observation) return { Error::sourceConflict, "source_changed_during_resolution" };
        }
        if (cancelled && cancelled->load(std::memory_order_acquire)) return { Error::cancelled, "source_resolution_cancelled" };
        if (request.expectedObservation && candidate->observation != *request.expectedObservation)
            return { Error::sourceConflict, "source_review_obsolete" };
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "source_resolution_exception" }; }
}

Status RecoverySourceBinding::check(const media::SourceIdentity& source, const std::atomic<bool>* cancelled) const
{
    if (cancelled && cancelled->load(std::memory_order_acquire)) return { Error::cancelled, "source_resolution_cancelled" };
    if (!source.sameBinding(original) || source.location.file != original.location.file || source.sizeBytes != original.sizeBytes
        || source.modifiedMilliseconds != original.modifiedMilliseconds || source.fastStampValid != original.fastStampValid)
        return { Error::sourceConflict, "source_binding_changed" };
    if (observation.location == juce::File()) return {};
    const auto current = RecoverySourceIdentity::observe(observation.location, cancelled);
    if (!current.status) return current.status;
    return current.observation == observation ? Status{} : Status { Error::sourceConflict, "source_changed_during_attempt" };
}
}
