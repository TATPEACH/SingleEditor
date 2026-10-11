#pragma once

#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "AudioSuite/ParametricEqProcessor.h"
#include "AudioSuite/TruePeakLimiterProcessor.h"

#include <juce_core/juce_core.h>

#include <variant>

namespace singleeditor
{
using CliAudioSuitePlanVariant = std::variant<std::monostate,
                                               AudioSuitePlanPtr,
                                               ParametricEqPlanPtr,
                                               TruePeakLimiterPlanPtr>;

struct CliAudioSuiteAnalyzeResult
{
    juce::String operationId;
    juce::String operationVersion;
    juce::String algorithmVersion;
    juce::var canonicalParameters;
    ResolvedAudioSelection selection;
    CliAudioSuitePlanVariant plan;
    juce::String errorMessage;

    bool succeeded() const noexcept
    {
        return !std::holds_alternative<std::monostate>(plan);
    }
};

// CLI-only typed adapter over the production AudioSuite processors. Discovery
// calls only cold schema helpers; processor factories are used solely by an
// explicit Analyze or Apply.
class CliAudioSuiteOperationRegistry final
{
public:
    static juce::String canonicalOperationId(const juce::String& idOrAlias);
    static bool validateCanonicalParameters(const juce::String& operationId,
                                            const juce::var& parameters,
                                            juce::var& canonicalParameters,
                                            juce::String& error);
    static CliAudioSuiteAnalyzeResult analyze(
        const juce::String& operationId,
        SingleTrackDocument::StatePtr state,
        int64_t documentGeneration,
        const AudioSelectionRequest& selection,
        const juce::var& parameters);
    static AudioSuiteApplyResult apply(
        const CliAudioSuiteAnalyzeResult& analyzed,
        SingleTrackDocument& document,
        int64_t currentDocumentGeneration,
        const AudioSuiteCancellationToken& cancellation = {});
    static AudioSuitePlanPtr basePlan(const CliAudioSuiteAnalyzeResult& analyzed);
    static int liveProcessorInstanceCountForDiagnostics() noexcept;
};
} // namespace singleeditor
