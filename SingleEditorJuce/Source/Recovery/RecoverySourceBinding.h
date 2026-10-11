#pragma once
#include "RecoverySourceIdentity.h"

namespace singleeditor::recovery
{
// Worker-owned result of the real D187 resolver. Its within-attempt checks are
// bounded metadata observations; hashing occurs once during explicit resolve.
// No live source handle/decoder/write denial is retained while materialising.
class RecoverySourceBinding final
{
public:
    struct Request final
    {
        juce::File locatedFile; // Empty means the recorded path; never scans.
        bool relocationConfirmed = false;
        RecoverySourceIdentity::Mode verification = RecoverySourceIdentity::Mode::sparseAfterMutation;
        // Optional explicit review fence. A later Restore cannot silently use
        // a different physical source than the one the user just inspected.
        std::optional<SourceObservation> expectedObservation;
    };
    static Status resolve(const media::SourceIdentity&, const std::optional<RecoverySourceEvidence>&,
                          const Request&, std::unique_ptr<RecoverySourceBinding>&,
                          const std::atomic<bool>* cancelled = nullptr);
    Status check(const media::SourceIdentity&, const std::atomic<bool>* cancelled = nullptr) const;
    const SourceResolution& getResolution() const noexcept { return resolution; }
    const SourceObservation& getObservation() const noexcept { return observation; }
    bool requiresSaveAs() const noexcept { return !resolution.permitsReplay(relocationConfirmed); }
    size_t payloadBytesRead() const noexcept { return sparseBytes; }
    int64_t fullBytesRead() const noexcept { return deepBytes; }
    const juce::String& fullFileHash() const noexcept { return deepHash; }
private:
    media::SourceIdentity original;
    SourceObservation observation;
    SourceResolution resolution;
    bool relocationConfirmed = false;
    size_t sparseBytes = 0;
    int64_t deepBytes = 0;
    juce::String deepHash;
};
}
