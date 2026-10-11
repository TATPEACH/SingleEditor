#pragma once

#include "Document/SingleTrackDocument.h"
#include "Waveform/WaveformModel.h"

namespace singleeditor
{
namespace recovery { struct RecoveredCandidateDocument; }

// A ready candidate's independent playlist lifetime. Creation does not select
// it, start playback, schedule work or read its historical source path.
class RecoveredPlaylistDocument final
{
public:
    using Ptr = std::shared_ptr<RecoveredPlaylistDocument>;
    static Ptr fromCandidate(const recovery::RecoveredCandidateDocument&);
    juce::Uuid attemptId() const noexcept { return attempt; }
    const SingleTrackDocument::WorkspaceSnapshot::Ptr& snapshot() const noexcept { return image; }
    const SingleTrackDocument::StatePtr& originalCoreState() const noexcept { return coreState; }

private:
    friend class WorkspaceSessionSupport;
    RecoveredPlaylistDocument() = default;
    juce::Uuid attempt;
    SingleTrackDocument::WorkspaceSnapshot::Ptr image;
    SingleTrackDocument::StatePtr coreState;
    std::shared_ptr<WaveformModel::PeakCache> peaks;
    std::shared_ptr<WaveformModel::SpectrogramCache> spectrogram;
    bool acceptsCorePeaks = true;
};
}
