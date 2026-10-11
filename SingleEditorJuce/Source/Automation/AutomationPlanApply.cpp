#include "Automation/AutomationPlanApply.h"

#include "Automation/AutomationPlanIO.h"
#include "Automation/AutomationPlanPreflight.h"
#include "Automation/EditPipelineAnalysisService.h"
#include "Automation/FileIdentity.h"
#include "Document/SingleTrackDocument.h"

#include <cmath>

namespace singleeditor
{
namespace
{
juce::File makeUniqueBackupFile(const juce::File& input,
                                const juce::File& requestedDirectory)
{
    const auto directory = requestedDirectory == juce::File()
                         ? input.getParentDirectory()
                         : requestedDirectory;
    const auto baseName = input.getFileName() + ".bak";
    auto candidate = directory.getChildFile(baseName);
    for (int suffix = 1; candidate.exists(); ++suffix)
        candidate = directory.getChildFile(baseName + "." + juce::String(suffix));
    return candidate;
}

juce::File makeRenamedOutput(const juce::File& requested)
{
    auto candidate = requested;
    for (int suffix = 1; candidate.exists(); ++suffix)
    {
        candidate = requested.getSiblingFile(
            requested.getFileNameWithoutExtension() + "-" + juce::String(suffix)
            + requested.getFileExtension());
    }
    return candidate;
}

bool isLossyExtension(const juce::String& extension) noexcept
{
    const auto lower = extension.toLowerCase();
    return lower == ".mp3" || lower == ".ogg";
}

bool samePath(const juce::File& left, const juce::File& right)
{
#if JUCE_WINDOWS
    return left.getFullPathName().equalsIgnoreCase(right.getFullPathName());
#else
    return left == right;
#endif
}

juce::DynamicObject* makeSummary(int total,
                                 int committed,
                                 int skipped,
                                 int failed)
{
    auto* summary = new juce::DynamicObject();
    summary->setProperty("total", total);
    summary->setProperty("committed", committed);
    summary->setProperty("skipped", skipped);
    summary->setProperty("failed", failed);
    return summary;
}

void failItem(juce::DynamicObject& result,
              const juce::String& code,
              const juce::String& message)
{
    result.setProperty("status", "failed");
    result.setProperty("error_code", code);
    result.setProperty("error", message);
    result.setProperty("write_performed", false);
}
} // namespace

AutomationPlanApplyResult AutomationPlanApply::run(
    const juce::File& planFile,
    const AutomationPlanApplyOptions& options)
{
    AutomationPlanApplyResult result;

    const auto preflight = AutomationPlanPreflight::run(planFile);
    juce::var plan;
    juce::String readError;
    if (!readAutomationPlan(planFile, plan, readError))
    {
        result.exitCode = AutomationExitCode::invalidArguments;
        result.errorMessage = readError;
        return result;
    }
    result.operationId = plan["operation"]["id"].toString();

    const auto* items = plan["items"].getArray();
    const auto* preflightResults = preflight.payload["results"].getArray();
    if (items == nullptr || preflightResults == nullptr
        || items->size() != preflightResults->size())
    {
        result.exitCode = preflight.exitCode;
        result.errorMessage = preflight.errorMessage.isNotEmpty()
                                ? preflight.errorMessage
                                : "Plan preflight results are inconsistent";
        result.payload = preflight.payload;
        return result;
    }

    juce::Array<juce::var> itemResults;
    int committed = 0;
    int skipped = 0;
    int failed = 0;
    bool writePerformed = false;

    for (int index = 0; index < items->size(); ++index)
    {
        const auto item = items->getReference(index);
        const auto preflightItem = preflightResults->getReference(index);
        const auto input = juce::File(item["input"].toString());
        auto* itemResult = new juce::DynamicObject();
        itemResult->setProperty("input", input.getFullPathName());
        itemResult->setProperty("write_performed", false);

        if (preflightItem["status"].toString() != "ready")
        {
            failItem(*itemResult,
                     preflightItem["error_code"].toString(),
                     preflightItem["error"].toString());
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        if (item["status"].toString() == "skipped")
        {
            itemResult->setProperty("status", "skipped");
            itemResult->setProperty("error_code", "NO_CHANGES");
            itemResult->setProperty("error", "Analysis resolved to no audio changes");
            ++skipped;
            itemResults.add(juce::var(itemResult));
            continue;
        }

        auto output = options.inPlace
                    ? input
                    : options.outputDirectory.getChildFile(input.getFileName());
        itemResult->setProperty("output", output.getFullPathName());
        if (!options.inPlace && samePath(input, output))
        {
            failItem(*itemResult, "OUTPUT_EQUALS_INPUT",
                     "--output-dir resolves to the input path; use --in-place");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        if (!options.inPlace && output.existsAsFile())
        {
            if (options.conflictPolicy == "skip")
            {
                itemResult->setProperty("status", "skipped");
                itemResult->setProperty("error_code", "OUTPUT_EXISTS");
                itemResult->setProperty("error", "Output exists and conflict policy is skip");
                ++skipped;
                itemResults.add(juce::var(itemResult));
                continue;
            }
            if (options.conflictPolicy == "rename")
            {
                output = makeRenamedOutput(output);
                itemResult->setProperty("output", output.getFullPathName());
            }
            else if (options.conflictPolicy != "overwrite")
            {
                failItem(*itemResult, "OUTPUT_EXISTS",
                         "Output exists and conflict policy is fail");
                ++failed;
                itemResults.add(juce::var(itemResult));
                if (options.failFast)
                    break;
                continue;
            }
        }

        if (options.inPlace && isLossyExtension(input.getFileExtension())
            && !options.allowLossyReencode)
        {
            failItem(*itemResult, "LOSSY_REENCODE_REQUIRES_CONSENT",
                     "In-place MP3/OGG apply requires --allow-lossy-reencode");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        if (const auto* unsupported = item["metadata"]["unsupported"].getArray();
            unsupported != nullptr && !unsupported->isEmpty())
        {
            failItem(*itemResult, "METADATA_UNSUPPORTED",
                     "Plan contains metadata that cannot be preserved safely");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        FileIdentity expectedIdentity;
        juce::String identityError;
        if (!FileIdentity::fromJson(item["input_identity"], expectedIdentity,
                                    identityError)
            || !expectedIdentity.matches(FileIdentity::capture(input)))
        {
            failItem(*itemResult, "STALE_INPUT",
                     identityError.isNotEmpty() ? identityError
                                                : "Input changed before render");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        SingleTrackDocument document;
        if (!document.loadFile(input))
        {
            failItem(*itemResult, "OPEN_FAILED", "Input could not be opened for apply");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        if (result.operationId == "trim-silence.v1")
        {
            const auto sampleRate = document.getSampleRate();
            const auto range = item["suggested_sample_range"];
            const auto startSample = static_cast<juce::int64>(range["start_sample"]);
            const auto endSample = static_cast<juce::int64>(range["end_sample"]);
            if (endSample > startSample)
            {
                const juce::Range<double> seconds(
                    static_cast<double>(startSample) / sampleRate,
                    static_cast<double>(endSample) / sampleRate);
                if (!document.deleteSelection(seconds))
                {
                    failItem(*itemResult, "RENDER_FAILED",
                             "Planned trim could not be applied");
                    ++failed;
                    itemResults.add(juce::var(itemResult));
                    if (options.failFast)
                        break;
                    continue;
                }
            }
        }
        else
        {
            juce::String pipelineError;
            SingleTrackDocument::OfflineEditPipeline pipeline;
            if (!EditPipelineAnalysisService::validateResolvedPipeline(
                    item["render_pipeline"], document.getNumSamples(), pipelineError)
                || !EditPipelineAnalysisService::buildOfflinePipeline(
                    item["render_pipeline"], pipeline, pipelineError)
                || !document.applyOfflineEditPipeline(pipeline))
            {
                failItem(*itemResult, "RENDER_FAILED",
                         pipelineError.isNotEmpty()
                             ? pipelineError
                             : "Resolved edit pipeline could not be applied");
                ++failed;
                itemResults.add(juce::var(itemResult));
                if (options.failFast)
                    break;
                continue;
            }
        }

        juce::File backup;
        const auto targetExisted = output.existsAsFile();
        if (options.backup && targetExisted)
        {
            backup = makeUniqueBackupFile(output, options.backupDirectory);
            if (!backup.getParentDirectory().createDirectory()
                || !output.copyFileTo(backup)
                || backup.getSize() != output.getSize())
            {
                backup.deleteFile();
                failItem(*itemResult, "BACKUP_FAILED", "Could not create a verified backup");
                ++failed;
                itemResults.add(juce::var(itemResult));
                if (options.failFast)
                    break;
                continue;
            }
            itemResult->setProperty("backup", backup.getFullPathName());
        }
        else
        {
            itemResult->setProperty("backup", juce::var());
        }

        if (!expectedIdentity.matches(FileIdentity::capture(input)))
        {
            failItem(*itemResult, "STALE_INPUT", "Input changed before atomic commit");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        const auto saved = options.inPlace
                         ? document.saveOverSourceFile()
                         : document.saveToFile(output);
        if (!saved)
        {
            failItem(*itemResult, "WRITE_FAILED", "Rendered output could not be committed");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        const auto expectedSamples = static_cast<juce::int64>(
            item["estimated_output"]["num_samples"]);
        const auto expectedRate = static_cast<double>(item["media"]["sample_rate"]);
        const auto expectedChannels = static_cast<int>(item["media"]["num_channels"]);
        const auto outputInfo = SingleTrackDocument::probeFileInfo(output);
        if (!outputInfo.isValid()
            || outputInfo.numSamples != expectedSamples
            || outputInfo.numChannels != expectedChannels
            || std::abs(outputInfo.sampleRate - expectedRate) > 0.001)
        {
            failItem(*itemResult, "VERIFY_FAILED",
                     "Committed output failed structural verification");
            ++failed;
            itemResults.add(juce::var(itemResult));
            if (options.failFast)
                break;
            continue;
        }

        itemResult->setProperty("status", "committed");
        itemResult->setProperty("error_code", juce::var());
        itemResult->setProperty("error", juce::var());
        itemResult->setProperty("write_performed", true);
        itemResult->setProperty("output_identity", FileIdentity::capture(output).toJson());
        itemResult->setProperty("num_samples", outputInfo.numSamples);
        itemResult->setProperty("metadata_policy", "preserve_or_reject");
        ++committed;
        writePerformed = true;
        itemResults.add(juce::var(itemResult));
    }

    if (failed == 0)
        result.exitCode = AutomationExitCode::success;
    else if (committed > 0 || skipped > 0)
        result.exitCode = AutomationExitCode::partialFailure;
    else if (preflight.exitCode == AutomationExitCode::staleInput)
        result.exitCode = AutomationExitCode::staleInput;
    else
        result.exitCode = AutomationExitCode::processingFailed;

    if (failed > 0)
        result.errorMessage = "One or more plan items could not be applied";

    auto* payload = new juce::DynamicObject();
    payload->setProperty("schema", "single-editor.apply-manifest.v1");
    payload->setProperty("schema_version", 1);
    payload->setProperty("output_policy", options.inPlace ? "in_place" : "output_directory");
    payload->setProperty("write_performed", writePerformed);
    payload->setProperty("results", juce::var(itemResults));
    payload->setProperty("summary", juce::var(makeSummary(items->size(), committed,
                                                           skipped, failed)));
    result.payload = juce::var(payload);

    if (options.manifestFile != juce::File())
    {
        juce::String manifestError;
        if (!writeAutomationPlanAtomically(options.manifestFile, result.payload,
                                           manifestError))
        {
            result.exitCode = writePerformed ? AutomationExitCode::partialFailure
                                             : AutomationExitCode::processingFailed;
            result.errorMessage = manifestError;
        }
    }
    return result;
}
} // namespace singleeditor
