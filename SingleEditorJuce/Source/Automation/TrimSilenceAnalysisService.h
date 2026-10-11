#pragma once

#include "Automation/FileIdentity.h"
#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
struct TrimSilenceAnalysisOptions
{
    bool headOnly = true;
    double thresholdDbfs = -60.0;
    double windowMilliseconds = 10.0;
    double confirmationMilliseconds = 20.0;
    double hysteresisDb = 6.0;
    double prerollMilliseconds = 5.0;
};

struct TrimSilenceAnalysisResult
{
    bool succeeded = false;
    bool contentDetected = false;
    FileIdentity identity;
    SingleTrackDocument::FileInfo info;
    juce::int64 contentStartSample = 0;
    juce::int64 suggestedTrimEndSample = 0;
    juce::int64 samplesScanned = 0;
    juce::String errorMessage;

    juce::var toPlanItem(const TrimSilenceAnalysisOptions& options) const;
};

class TrimSilenceAnalysisService final
{
public:
    static TrimSilenceAnalysisResult analyze(const juce::File& file,
                                             TrimSilenceAnalysisOptions options = {});
};
} // namespace singleeditor
