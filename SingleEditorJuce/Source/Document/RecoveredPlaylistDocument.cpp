#include "RecoveredPlaylistDocument.h"
#include "Recovery/RecoveryRestoreCandidate.h"
#include <cmath>

namespace singleeditor
{
RecoveredPlaylistDocument::Ptr RecoveredPlaylistDocument::fromCandidate(const recovery::RecoveredCandidateDocument& candidate)
{
    if (candidate.attempt.isNull() || !candidate.document || !candidate.document->hasUnsavedChanges()
        || !candidate.coreState || candidate.document->getState() != candidate.coreState
        || !candidate.coreState->info.recoveryBacked || !candidate.firstPeaks
        || candidate.firstPeaks->state != candidate.coreState
        || candidate.firstPeaks->audioRevision != candidate.coreState->audioRevision
        || candidate.firstPeaks->channelCount != candidate.coreState->info.numChannels
        || candidate.firstPeaks->totalSamples != candidate.coreState->info.numSamples
        || std::abs(candidate.firstPeaks->sampleRate - candidate.coreState->info.sampleRate) > 0.0001
        || std::abs(candidate.firstPeaks->durationSeconds - candidate.coreState->info.durationSeconds) > 0.0001
        || candidate.firstPeaks->binCount <= 0 || candidate.firstPeaks->loadedSampleRange.isEmpty()
        || candidate.firstPeaks->loadedSampleRange.getStart() != 0
        || candidate.firstPeaks->loadedSampleRange.getEnd() > candidate.coreState->getPlayableSampleRange().getEnd())
        return {};
    auto image = candidate.document->captureWorkspaceSnapshot();
    if (!image) return {};
    auto result = Ptr(new RecoveredPlaylistDocument());
    result->attempt = candidate.attempt;
    result->image = std::move(image);
    result->coreState = candidate.coreState;
    // The core packet has handed these peaks to its single UI owner. The
    // candidate's worker subsequently reads coreState, never this cache metadata.
    result->peaks = candidate.firstPeaks;
    return result;
}
}
