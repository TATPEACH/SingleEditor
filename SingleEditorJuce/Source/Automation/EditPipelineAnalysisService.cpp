#include "Automation/EditPipelineAnalysisService.h"

#include "Automation/TrimSilenceAnalysisService.h"

#include <cmath>

namespace singleeditor
{
namespace
{
constexpr auto pipelineSchema = "single-editor.edit-pipeline.v1";
constexpr auto resolvedPipelineSchema = "single-editor.render-pipeline.v1";

bool isFiniteNumber(const juce::var& value, double& number)
{
    if (!(value.isInt() || value.isInt64() || value.isDouble()))
        return false;
    number = static_cast<double>(value);
    return std::isfinite(number);
}

bool hasOnlyProperties(const juce::DynamicObject& object,
                       const juce::StringArray& allowed,
                       juce::String& errorMessage)
{
    const auto& properties = object.getProperties();
    for (int index = 0; index < properties.size(); ++index)
    {
        const auto name = properties.getName(index).toString();
        if (!allowed.contains(name))
        {
            errorMessage = "Unknown pipeline property: " + name;
            return false;
        }
    }
    return true;
}

juce::var makePipeline(juce::Array<juce::var> operations)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("schema", pipelineSchema);
    root->setProperty("schemaVersion", 1);
    root->setProperty("operations", juce::var(std::move(operations)));
    return juce::var(root);
}

juce::var makeResolvedPipeline(juce::Array<juce::var> operations)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("schema", resolvedPipelineSchema);
    root->setProperty("schema_version", 1);
    root->setProperty("operations", juce::var(std::move(operations)));
    return juce::var(root);
}

juce::Array<juce::var> metadataRisksFor(
    const SingleTrackDocument::FileInfo& info)
{
    juce::Array<juce::var> risks;
    for (const auto& key : info.metadata.getAllKeys())
    {
        const auto lower = key.toLowerCase();
        if (lower.contains("bwf") || lower.contains("bwav")
            || lower.contains("ixml") || lower.contains("cue")
            || lower.contains("smpl") || lower.contains("loop"))
        {
            if (!risks.contains(key))
                risks.add(key);
        }
    }
    return risks;
}

double gainForResolvedOperation(const juce::var& operation,
                                juce::int64 sample) noexcept
{
    const auto id = operation["id"].toString();
    if (id == "gain.v1" || id == "normalize.peak.v1")
        return static_cast<double>(operation["linear_gain"]);
    if (id != "fade.v1")
        return 1.0;

    const auto start = static_cast<juce::int64>(operation["start_sample"]);
    const auto end = static_cast<juce::int64>(operation["end_sample"]);
    if (sample < start || sample >= end)
        return 1.0;
    const auto length = juce::jmax<juce::int64>(1, end - start - 1);
    const auto position = static_cast<double>(sample - start)
                        / static_cast<double>(length);
    return operation["direction"].toString() == "in"
         ? position
         : 1.0 - position;
}

bool scanPeak(const juce::File& file,
              juce::int64 sourceStart,
              juce::int64 outputSamples,
              const juce::Array<juce::var>& operations,
              double& peak,
              juce::String& errorMessage)
{
    auto prepared = SingleTrackDocument::prepareAudioFileForOpen(file);
    auto* reader = prepared.getReader();
    if (reader == nullptr || !prepared.info.isValid())
    {
        errorMessage = prepared.errorMessage.isNotEmpty()
                         ? prepared.errorMessage
                         : "No supported audio reader could scan the input";
        return false;
    }

    constexpr int blockSize = 65536;
    juce::AudioBuffer<float> block(prepared.info.numChannels, blockSize);
    peak = 0.0;
    for (juce::int64 outputStart = 0; outputStart < outputSamples;)
    {
        const auto count = static_cast<int>(
            juce::jmin<juce::int64>(blockSize, outputSamples - outputStart));
        block.clear();
        if (!reader->read(&block, 0, count, sourceStart + outputStart,
                          true, true))
        {
            errorMessage = "Audio read failed while resolving peak normalization";
            return false;
        }

        for (int channel = 0; channel < block.getNumChannels(); ++channel)
        {
            const auto* samples = block.getReadPointer(channel);
            for (int offset = 0; offset < count; ++offset)
            {
                auto gain = 1.0;
                const auto outputSample = outputStart + offset;
                for (const auto& operation : operations)
                    gain *= gainForResolvedOperation(operation, outputSample);
                peak = juce::jmax(peak,
                                  std::abs(static_cast<double>(samples[offset]))
                                      * gain);
            }
        }
        outputStart += count;
    }
    return true;
}

juce::var makeMedia(const SingleTrackDocument::FileInfo& info)
{
    auto* media = new juce::DynamicObject();
    media->setProperty("format", info.formatName);
    media->setProperty("sample_rate", info.sampleRate);
    media->setProperty("bits_per_sample", info.bitsPerSample);
    media->setProperty("num_channels", info.numChannels);
    media->setProperty("num_samples", info.numSamples);
    media->setProperty("duration_seconds", info.durationSeconds);
    auto* layout = new juce::DynamicObject();
    layout->setProperty("preset_id", info.channelLayout.selectedPresetId);
    layout->setProperty("display_name", info.channelLayout.displayName);
    layout->setProperty("order", info.channelLayout.orderText);
    layout->setProperty("detected_from_metadata",
                        info.channelLayout.detectedFromMetadata);
    media->setProperty("channel_layout", juce::var(layout));
    return juce::var(media);
}
} // namespace

EditPipelineValidationResult EditPipelineAnalysisService::validate(
    const juce::var& pipeline)
{
    EditPipelineValidationResult result;
    const auto* root = pipeline.getDynamicObject();
    if (root == nullptr)
    {
        result.errorMessage = "Pipeline root must be an object";
        return result;
    }

    if (!hasOnlyProperties(*root, { "schema", "schemaVersion", "operations" },
                           result.errorMessage))
        return result;
    const auto schema = root->getProperty("schema").toString();
    if (schema.isNotEmpty() && schema != pipelineSchema)
    {
        result.errorMessage = "Unsupported edit pipeline schema";
        return result;
    }
    if (static_cast<int>(root->getProperty("schemaVersion")) != 1)
    {
        result.errorMessage = "Edit pipeline requires schemaVersion 1";
        return result;
    }

    const auto* operations = root->getProperty("operations").getArray();
    if (operations == nullptr || operations->isEmpty() || operations->size() > 64)
    {
        result.errorMessage = "Edit pipeline requires 1 to 64 operations";
        return result;
    }

    juce::Array<juce::var> canonicalOperations;
    bool trimSeen = false;
    bool normalizeSeen = false;
    for (int index = 0; index < operations->size(); ++index)
    {
        const auto* operation = operations->getReference(index).getDynamicObject();
        if (operation == nullptr)
        {
            result.errorMessage = "Every pipeline operation must be an object";
            return result;
        }
        const auto id = operation->getProperty("id").toString();
        if (normalizeSeen)
        {
            result.errorMessage = "normalize.peak.v1 must be the final operation";
            return result;
        }

        auto* canonical = new juce::DynamicObject();
        canonical->setProperty("id", id);
        if (id == "gain.v1")
        {
            if (!hasOnlyProperties(*operation, { "id", "gainDb" },
                                   result.errorMessage))
                return result;
            double gainDb = 0.0;
            if (!isFiniteNumber(operation->getProperty("gainDb"), gainDb)
                || gainDb < -120.0 || gainDb > 24.0)
            {
                result.errorMessage = "gain.v1 gainDb must be from -120 to 24";
                return result;
            }
            canonical->setProperty("gainDb", gainDb);
        }
        else if (id == "fade.v1")
        {
            if (!hasOnlyProperties(*operation,
                                   { "id", "direction", "durationSeconds" },
                                   result.errorMessage))
                return result;
            const auto direction = operation->getProperty("direction").toString();
            double duration = 0.0;
            if (!juce::StringArray { "in", "out" }.contains(direction)
                || !isFiniteNumber(operation->getProperty("durationSeconds"), duration)
                || duration <= 0.0)
            {
                result.errorMessage = "fade.v1 requires in/out and a positive durationSeconds";
                return result;
            }
            canonical->setProperty("direction", direction);
            canonical->setProperty("durationSeconds", duration);
        }
        else if (id == "normalize.peak.v1")
        {
            if (!hasOnlyProperties(*operation, { "id", "targetDbfs" },
                                   result.errorMessage))
                return result;
            double target = 0.0;
            if (!isFiniteNumber(operation->getProperty("targetDbfs"), target)
                || target < -60.0 || target > 0.0)
            {
                result.errorMessage = "normalize.peak.v1 targetDbfs must be from -60 to 0";
                return result;
            }
            normalizeSeen = true;
            canonical->setProperty("targetDbfs", target);
        }
        else if (id == "trim-silence.v1")
        {
            if (index != 0 || trimSeen)
            {
                result.errorMessage = "trim-silence.v1 may appear only once at pipeline start";
                return result;
            }
            if (!hasOnlyProperties(*operation,
                                   { "id", "headOnly", "thresholdDbfs" },
                                   result.errorMessage))
                return result;
            const auto headOnly = !operation->hasProperty("headOnly")
                               || static_cast<bool>(operation->getProperty("headOnly"));
            double threshold = -60.0;
            const auto thresholdValue = operation->hasProperty("thresholdDbfs")
                                      ? operation->getProperty("thresholdDbfs")
                                      : juce::var(-60.0);
            if (!headOnly
                || !isFiniteNumber(thresholdValue, threshold)
                || threshold < -160.0 || threshold > 0.0)
            {
                result.errorMessage = "F2 trim-silence requires headOnly and -160..0 dBFS";
                return result;
            }
            trimSeen = true;
            canonical->setProperty("headOnly", true);
            canonical->setProperty("thresholdDbfs", threshold);
        }
        else
        {
            result.errorMessage = "Unsupported pipeline operation: " + id;
            return result;
        }
        canonicalOperations.add(juce::var(canonical));
    }

    result.succeeded = true;
    result.canonicalPipeline = makePipeline(std::move(canonicalOperations));
    return result;
}

EditPipelineAnalysisResult EditPipelineAnalysisService::analyze(
    const juce::File& file,
    const juce::var& pipeline)
{
    EditPipelineAnalysisResult result;
    result.identity = FileIdentity::capture(file);
    result.info.file = file;
    const auto validation = validate(pipeline);
    if (!validation.succeeded)
    {
        result.errorMessage = validation.errorMessage;
        return result;
    }

    auto prepared = SingleTrackDocument::prepareAudioFileForOpen(file);
    if (prepared.getReader() == nullptr || !prepared.info.isValid())
    {
        result.errorMessage = prepared.errorMessage.isNotEmpty()
                                ? prepared.errorMessage
                                : "No supported audio reader could open the input";
        return result;
    }
    result.info = prepared.info;

    const auto* requestedOperations = validation.canonicalPipeline["operations"].getArray();
    juce::Array<juce::var> resolvedOperations;
    for (const auto& operation : *requestedOperations)
    {
        if (operation["id"].toString() != "trim-silence.v1")
            continue;
        TrimSilenceAnalysisOptions options;
        options.headOnly = true;
        options.thresholdDbfs = static_cast<double>(operation["thresholdDbfs"]);
        const auto trim = TrimSilenceAnalysisService::analyze(file, options);
        if (!trim.succeeded)
        {
            result.errorMessage = trim.errorMessage;
            return result;
        }
        result.trimmedSamples = trim.suggestedTrimEndSample;
        result.hasChanges = result.hasChanges || result.trimmedSamples > 0;
        auto* resolved = new juce::DynamicObject();
        resolved->setProperty("id", "trim.range.v1");
        resolved->setProperty("start_sample", static_cast<juce::int64>(0));
        resolved->setProperty("end_sample", result.trimmedSamples);
        resolvedOperations.add(juce::var(resolved));
        break;
    }

    const auto outputSamples = result.info.numSamples - result.trimmedSamples;
    if (outputSamples <= 0)
    {
        result.errorMessage = "Pipeline would remove the entire input";
        return result;
    }

    for (const auto& operation : *requestedOperations)
    {
        const auto id = operation["id"].toString();
        if (id == "trim-silence.v1")
            continue;

        auto* resolved = new juce::DynamicObject();
        resolved->setProperty("id", id);
        if (id == "gain.v1")
        {
            const auto gainDb = static_cast<double>(operation["gainDb"]);
            result.hasChanges = result.hasChanges || std::abs(gainDb) > 1.0e-9;
            resolved->setProperty("gain_db", gainDb);
            resolved->setProperty("linear_gain", std::pow(10.0, gainDb / 20.0));
            resolved->setProperty("start_sample", static_cast<juce::int64>(0));
            resolved->setProperty("end_sample", outputSamples);
        }
        else if (id == "fade.v1")
        {
            result.hasChanges = true;
            const auto durationSamples = static_cast<juce::int64>(std::llround(
                static_cast<double>(operation["durationSeconds"])
                * result.info.sampleRate));
            if (durationSamples <= 0 || durationSamples > outputSamples)
            {
                result.errorMessage = "Fade duration is outside the output clip";
                return result;
            }
            const auto direction = operation["direction"].toString();
            resolved->setProperty("direction", direction);
            resolved->setProperty("start_sample",
                                  direction == "in" ? 0 : outputSamples - durationSamples);
            resolved->setProperty("end_sample",
                                  direction == "in" ? durationSamples : outputSamples);
        }
        else if (id == "normalize.peak.v1")
        {
            if (!scanPeak(file, result.trimmedSamples, outputSamples,
                          resolvedOperations, result.measuredPeak,
                          result.errorMessage))
                return result;
            const auto targetDbfs = static_cast<double>(operation["targetDbfs"]);
            const auto targetGain = std::pow(10.0, targetDbfs / 20.0);
            const auto linearGain = result.measuredPeak > 1.0e-12
                                  ? targetGain / result.measuredPeak
                                  : 1.0;
            if (!std::isfinite(linearGain) || linearGain > 1000000.0)
            {
                result.errorMessage = "Peak normalization requires excessive gain";
                return result;
            }
            result.resolvedPeak = result.measuredPeak * linearGain;
            result.hasChanges = result.hasChanges
                             || std::abs(linearGain - 1.0) > 1.0e-7;
            resolved->setProperty("target_dbfs", targetDbfs);
            resolved->setProperty("measured_peak", result.measuredPeak);
            resolved->setProperty("linear_gain", linearGain);
            resolved->setProperty("start_sample", static_cast<juce::int64>(0));
            resolved->setProperty("end_sample", outputSamples);
        }
        resolvedOperations.add(juce::var(resolved));
    }

    result.renderPipeline = makeResolvedPipeline(std::move(resolvedOperations));
    if (!validateResolvedPipeline(result.renderPipeline, result.info.numSamples,
                                  result.errorMessage))
        return result;
    result.succeeded = true;
    return result;
}

juce::var EditPipelineAnalysisResult::toPlanItem() const
{
    auto* item = new juce::DynamicObject();
    item->setProperty("input", info.file.getFullPathName());
    item->setProperty("input_identity", identity.toJson());
    item->setProperty("status",
                      succeeded ? (hasChanges ? "planned" : "skipped")
                                : "failed");
    item->setProperty("error",
                      errorMessage.isEmpty() ? juce::var() : juce::var(errorMessage));
    item->setProperty("media", makeMedia(info));

    auto* analysis = new juce::DynamicObject();
    analysis->setProperty("trimmed_samples", trimmedSamples);
    analysis->setProperty("measured_peak", measuredPeak);
    analysis->setProperty("resolved_peak", resolvedPeak);
    item->setProperty("analysis", juce::var(analysis));

    auto* range = new juce::DynamicObject();
    range->setProperty("start_sample", static_cast<juce::int64>(0));
    range->setProperty("end_sample", trimmedSamples);
    item->setProperty("suggested_sample_range", juce::var(range));
    item->setProperty("render_pipeline", renderPipeline);

    const auto outputSamples = juce::jmax<juce::int64>(
        0, info.numSamples - trimmedSamples);
    auto* estimated = new juce::DynamicObject();
    estimated->setProperty("num_samples", outputSamples);
    estimated->setProperty("duration_seconds",
                           info.sampleRate > 0.0
                               ? static_cast<double>(outputSamples) / info.sampleRate
                               : 0.0);
    estimated->setProperty("sample_rate", info.sampleRate);
    estimated->setProperty("bits_per_sample", info.bitsPerSample);
    estimated->setProperty("num_channels", info.numChannels);
    estimated->setProperty("format", info.formatName);
    item->setProperty("estimated_output", juce::var(estimated));

    auto* metadata = new juce::DynamicObject();
    metadata->setProperty("policy", "preserve_or_reject");
    metadata->setProperty("risks", juce::var(metadataRisksFor(info)));
    juce::Array<juce::var> unsupported;
    metadata->setProperty("unsupported", juce::var(unsupported));
    item->setProperty("metadata", juce::var(metadata));
    return juce::var(item);
}

bool EditPipelineAnalysisService::validateResolvedPipeline(
    const juce::var& pipeline,
    juce::int64 sourceSamples,
    juce::String& errorMessage)
{
    const auto* root = pipeline.getDynamicObject();
    if (root == nullptr
        || !hasOnlyProperties(*root, { "schema", "schema_version", "operations" },
                              errorMessage)
        || pipeline["schema"].toString() != resolvedPipelineSchema
        || static_cast<int>(pipeline["schema_version"]) != 1)
    {
        errorMessage = "Unsupported resolved render pipeline schema";
        return false;
    }
    const auto* operations = pipeline["operations"].getArray();
    if (operations == nullptr || operations->isEmpty() || operations->size() > 64)
    {
        errorMessage = "Resolved render pipeline requires 1 to 64 operations";
        return false;
    }

    auto outputSamples = sourceSamples;
    bool trimSeen = false;
    bool normalizeSeen = false;
    for (int index = 0; index < operations->size(); ++index)
    {
        const auto operation = operations->getReference(index);
        const auto* operationObject = operation.getDynamicObject();
        if (operationObject == nullptr)
        {
            errorMessage = "Resolved operation must be an object";
            return false;
        }
        const auto id = operation["id"].toString();
        if (id == "trim.range.v1")
        {
            if (!hasOnlyProperties(*operationObject,
                                   { "id", "start_sample", "end_sample" },
                                   errorMessage))
                return false;
            const auto start = static_cast<juce::int64>(operation["start_sample"]);
            const auto end = static_cast<juce::int64>(operation["end_sample"]);
            if (index != 0 || trimSeen || start < 0 || end < start
                || end > sourceSamples || end - start >= sourceSamples)
            {
                errorMessage = "Resolved trim range is invalid";
                return false;
            }
            trimSeen = true;
            outputSamples -= end - start;
            continue;
        }
        if (!juce::StringArray { "gain.v1", "fade.v1", "normalize.peak.v1" }
                 .contains(id))
        {
            errorMessage = "Unsupported resolved operation: " + id;
            return false;
        }
        if (normalizeSeen)
        {
            errorMessage = "Resolved peak normalize must be the final operation";
            return false;
        }
        if (id == "gain.v1"
            && !hasOnlyProperties(*operationObject,
                                  { "id", "gain_db", "linear_gain",
                                    "start_sample", "end_sample" },
                                  errorMessage))
            return false;
        if (id == "fade.v1"
            && !hasOnlyProperties(*operationObject,
                                  { "id", "direction", "start_sample",
                                    "end_sample" },
                                  errorMessage))
            return false;
        if (id == "normalize.peak.v1")
        {
            if (index != operations->size() - 1
                || !hasOnlyProperties(*operationObject,
                                      { "id", "target_dbfs", "measured_peak",
                                        "linear_gain", "start_sample", "end_sample" },
                                      errorMessage))
            {
                if (errorMessage.isEmpty())
                    errorMessage = "Resolved peak normalize must be the final operation";
                return false;
            }
            normalizeSeen = true;
        }
        const auto start = static_cast<juce::int64>(operation["start_sample"]);
        const auto end = static_cast<juce::int64>(operation["end_sample"]);
        if (start < 0 || end <= start || end > outputSamples)
        {
            errorMessage = "Resolved amplitude range is invalid";
            return false;
        }
        if (id == "fade.v1"
            && !juce::StringArray { "in", "out" }
                    .contains(operation["direction"].toString()))
        {
            errorMessage = "Resolved fade direction is invalid";
            return false;
        }
        if (id != "fade.v1")
        {
            double linearGain = 0.0;
            if (!isFiniteNumber(operation["linear_gain"], linearGain)
                || linearGain < 0.0)
            {
                errorMessage = "Resolved linear gain is invalid";
                return false;
            }
        }
    }
    return true;
}

bool EditPipelineAnalysisService::buildOfflinePipeline(
    const juce::var& resolvedPipeline,
    SingleTrackDocument::OfflineEditPipeline& pipeline,
    juce::String& errorMessage)
{
    const auto* operations = resolvedPipeline["operations"].getArray();
    if (operations == nullptr)
    {
        errorMessage = "Resolved render pipeline has no operations";
        return false;
    }

    pipeline = {};
    for (const auto& operation : *operations)
    {
        const auto id = operation["id"].toString();
        const juce::Range<juce::int64> range(
            static_cast<juce::int64>(operation["start_sample"]),
            static_cast<juce::int64>(operation["end_sample"]));
        if (id == "trim.range.v1")
        {
            pipeline.deletionRange = range;
            continue;
        }

        SingleTrackDocument::OfflineAmplitudeEdit edit;
        edit.sampleRange = range;
        if (id == "fade.v1")
        {
            edit.kind = operation["direction"].toString() == "in"
                      ? SingleTrackDocument::OfflineAmplitudeEdit::Kind::fadeIn
                      : SingleTrackDocument::OfflineAmplitudeEdit::Kind::fadeOut;
        }
        else if (id == "gain.v1" || id == "normalize.peak.v1")
        {
            edit.kind = SingleTrackDocument::OfflineAmplitudeEdit::Kind::gain;
            edit.linearGain = static_cast<float>(
                static_cast<double>(operation["linear_gain"]));
        }
        else
        {
            errorMessage = "Unsupported resolved operation: " + id;
            return false;
        }
        pipeline.amplitudeEdits.push_back(edit);
    }
    return true;
}
} // namespace singleeditor
