#pragma once

#include "Automation/FileIdentity.h"
#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
struct EditPipelineValidationResult
{
    bool succeeded = false;
    juce::var canonicalPipeline;
    juce::String errorMessage;
};

struct EditPipelineAnalysisResult
{
    bool succeeded = false;
    bool hasChanges = false;
    FileIdentity identity;
    SingleTrackDocument::FileInfo info;
    juce::var renderPipeline;
    juce::int64 trimmedSamples = 0;
    double measuredPeak = 0.0;
    double resolvedPeak = 0.0;
    juce::String errorMessage;

    juce::var toPlanItem() const;
};

class EditPipelineAnalysisService final
{
public:
    static EditPipelineValidationResult validate(const juce::var& pipeline);
    static EditPipelineAnalysisResult analyze(const juce::File& file,
                                               const juce::var& pipeline);
    static bool validateResolvedPipeline(const juce::var& pipeline,
                                         juce::int64 sourceSamples,
                                         juce::String& errorMessage);
    static bool buildOfflinePipeline(
        const juce::var& resolvedPipeline,
        SingleTrackDocument::OfflineEditPipeline& pipeline,
        juce::String& errorMessage);
};
} // namespace singleeditor
