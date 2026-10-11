#include "CLI/CliAudioSuiteOperationRegistry.h"

#include "CLI/CliColdManifest.h"

#include <algorithm>
#include <cmath>

namespace singleeditor
{
namespace
{
bool hasExactProperties(const juce::DynamicObject& object,
                        std::initializer_list<const char*> names)
{
    const auto& properties = object.getProperties();
    if (properties.size() != static_cast<int>(names.size()))
        return false;
    return std::all_of(names.begin(), names.end(), [&properties](const auto* name)
    {
        return properties.contains(juce::Identifier(name));
    });
}

bool finiteNumber(const juce::var& value, double& result) noexcept
{
    if (!(value.isDouble() || value.isInt() || value.isInt64()))
        return false;
    result = static_cast<double>(value);
    return std::isfinite(result);
}

juce::String filterTypeToString(ParametricEqFilterType type)
{
    switch (type)
    {
        case ParametricEqFilterType::highPass: return "high-pass";
        case ParametricEqFilterType::lowShelf: return "low-shelf";
        case ParametricEqFilterType::bell: return "bell";
        case ParametricEqFilterType::notch: return "notch";
        case ParametricEqFilterType::highShelf: return "high-shelf";
        case ParametricEqFilterType::lowPass: return "low-pass";
    }
    return {};
}

bool parseFilterType(const juce::String& value, ParametricEqFilterType& type)
{
    if (value == "high-pass") type = ParametricEqFilterType::highPass;
    else if (value == "low-shelf") type = ParametricEqFilterType::lowShelf;
    else if (value == "bell") type = ParametricEqFilterType::bell;
    else if (value == "notch") type = ParametricEqFilterType::notch;
    else if (value == "high-shelf") type = ParametricEqFilterType::highShelf;
    else if (value == "low-pass") type = ParametricEqFilterType::lowPass;
    else return false;
    return true;
}

juce::var canonicalReferenceGain(double gainDb, double linearGain)
{
    auto* result = new juce::DynamicObject();
    result->setProperty("gain_db", gainDb);
    result->setProperty("linear_gain", linearGain);
    return juce::var(result);
}

bool parseReferenceGain(const juce::var& value,
                        double& gainDb,
                        juce::var& canonical,
                        juce::String& error)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr
        || !(hasExactProperties(*object, { "gain_db" })
             || hasExactProperties(*object, { "gain_db", "linear_gain" })))
    {
        error = "Reference Gain parameters must contain gain_db and optional resolved linear_gain";
        return false;
    }
    if (!finiteNumber(object->getProperty("gain_db"), gainDb)
        || gainDb < -120.0 || gainDb > 24.0)
    {
        error = "gain_db is outside the advertised -120..24 dB range";
        return false;
    }
    const auto expected = std::pow(10.0, gainDb / 20.0);
    const auto tolerance = juce::jmax(1.0e-12, std::abs(expected) * 1.0e-6);
    auto resolved = expected;
    if (object->getProperties().contains("linear_gain"))
    {
        double supplied = 0.0;
        if (!finiteNumber(object->getProperty("linear_gain"), supplied)
            || supplied <= 0.0 || std::abs(supplied - expected) > tolerance)
        {
            error = "linear_gain does not match gain_db";
            return false;
        }
        resolved = supplied;
    }
    canonical = canonicalReferenceGain(gainDb, resolved);
    return true;
}

juce::var canonicalEq(const ParametricEqParameters& parameters)
{
    juce::Array<juce::var> bands;
    for (std::size_t index = 0; index < parameters.bands.size(); ++index)
    {
        const auto& source = parameters.bands[index];
        auto* band = new juce::DynamicObject();
        band->setProperty("index", static_cast<int>(index));
        band->setProperty("enabled", source.enabled);
        band->setProperty("type", filterTypeToString(source.type));
        band->setProperty("frequency_hz", source.frequencyHz);
        band->setProperty("gain_db", source.gainDb);
        band->setProperty("q", source.q);
        band->setProperty("slope",
                          source.slope == ParametricEqSlope::db24 ? "db24" : "db12");
        bands.add(juce::var(band));
    }
    auto* result = new juce::DynamicObject();
    result->setProperty("preset_id", parameters.presetId);
    result->setProperty("bands", juce::var(bands));
    return juce::var(result);
}

bool parseEq(const juce::var& value,
             ParametricEqParameters& parameters,
             juce::var& canonical,
             juce::String& error)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr
        || !hasExactProperties(*object, { "preset_id", "bands" }))
    {
        error = "Parametric EQ parameters must contain preset_id and bands";
        return false;
    }
    parameters.presetId = object->getProperty("preset_id").toString();
    if (parameters.presetId.isEmpty() || parameters.presetId.length() > 128)
    {
        error = "Parametric EQ preset_id is invalid";
        return false;
    }
    const auto* bands = object->getProperty("bands").getArray();
    if (bands == nullptr
        || bands->size() != static_cast<int>(ParametricEqParameters::bandCount))
    {
        error = "Parametric EQ requires exactly eight canonical bands";
        return false;
    }
    for (int index = 0; index < bands->size(); ++index)
    {
        const auto* band = bands->getReference(index).getDynamicObject();
        if (band == nullptr
            || !hasExactProperties(*band,
                                   { "index", "enabled", "type", "frequency_hz",
                                     "gain_db", "q", "slope" })
            || static_cast<int>(band->getProperty("index")) != index
            || !band->getProperty("enabled").isBool())
        {
            error = "Parametric EQ band structure/order is invalid";
            return false;
        }
        auto& target = parameters.bands[static_cast<std::size_t>(index)];
        target.enabled = static_cast<bool>(band->getProperty("enabled"));
        if (!parseFilterType(band->getProperty("type").toString(), target.type))
        {
            error = "Parametric EQ filter type is invalid";
            return false;
        }
        if (!finiteNumber(band->getProperty("frequency_hz"), target.frequencyHz)
            || !finiteNumber(band->getProperty("gain_db"), target.gainDb)
            || !finiteNumber(band->getProperty("q"), target.q)
            || target.frequencyHz <= 0.0 || target.q <= 0.0
            || std::abs(target.gainDb) > 48.0)
        {
            error = "Parametric EQ numeric parameter is invalid";
            return false;
        }
        const auto slope = band->getProperty("slope").toString();
        if (slope == "db12") target.slope = ParametricEqSlope::db12;
        else if (slope == "db24") target.slope = ParametricEqSlope::db24;
        else
        {
            error = "Parametric EQ slope is invalid";
            return false;
        }
    }
    canonical = canonicalEq(parameters);
    return true;
}

juce::var canonicalLimiter(const TruePeakLimiterParameters& parameters)
{
    auto* result = new juce::DynamicObject();
    result->setProperty("enabled", parameters.enabled);
    result->setProperty("threshold_db", parameters.thresholdDb);
    result->setProperty("ceiling_dbtp", parameters.ceilingDbtp);
    result->setProperty("lookahead_ms", parameters.lookAheadMs);
    result->setProperty("release_mode",
                        parameters.releaseMode == TruePeakLimiterReleaseMode::automatic
                            ? "automatic" : "manual");
    result->setProperty("release_ms", parameters.releaseMs);
    result->setProperty("oversampling", "sample-rate-aware-fixed");
    result->setProperty("channels_linked", parameters.channelsLinked);
    return juce::var(result);
}

bool parseLimiter(const juce::var& value,
                  TruePeakLimiterParameters& parameters,
                  juce::var& canonical,
                  juce::String& error)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr
        || !hasExactProperties(*object,
                               { "enabled", "threshold_db", "ceiling_dbtp",
                                 "lookahead_ms", "release_mode", "release_ms",
                                 "oversampling", "channels_linked" })
        || !object->getProperty("enabled").isBool()
        || !object->getProperty("channels_linked").isBool())
    {
        error = "True-Peak Limiter parameters do not match the canonical schema";
        return false;
    }
    parameters.enabled = static_cast<bool>(object->getProperty("enabled"));
    parameters.channelsLinked = static_cast<bool>(object->getProperty("channels_linked"));
    if (!finiteNumber(object->getProperty("threshold_db"), parameters.thresholdDb)
        || !finiteNumber(object->getProperty("ceiling_dbtp"), parameters.ceilingDbtp)
        || !finiteNumber(object->getProperty("lookahead_ms"), parameters.lookAheadMs)
        || !finiteNumber(object->getProperty("release_ms"), parameters.releaseMs))
    {
        error = "True-Peak Limiter numeric parameter is invalid";
        return false;
    }
    const auto releaseMode = object->getProperty("release_mode").toString();
    if (releaseMode == "automatic")
        parameters.releaseMode = TruePeakLimiterReleaseMode::automatic;
    else if (releaseMode == "manual")
        parameters.releaseMode = TruePeakLimiterReleaseMode::manual;
    else
    {
        error = "True-Peak Limiter release_mode is invalid";
        return false;
    }
    parameters.oversampling = TruePeakLimiterOversampling::sampleRateAwareFixed;
    if (!parameters.enabled
        || parameters.thresholdDb < -36.0 || parameters.thresholdDb > 0.0
        || parameters.ceilingDbtp < -12.0 || parameters.ceilingDbtp > 0.0
        || std::abs(parameters.lookAheadMs - 5.0) > 1.0e-9
        || parameters.releaseMs < 20.0 || parameters.releaseMs > 2000.0
        || object->getProperty("oversampling").toString()
               != "sample-rate-aware-fixed"
        || !parameters.channelsLinked)
    {
        error = "True-Peak Limiter parameters violate the v1 processor contract";
        return false;
    }
    canonical = canonicalLimiter(parameters);
    return true;
}

bool canonicalValuesMatch(const juce::var& left, const juce::var& right)
{
    return juce::JSON::toString(left, true) == juce::JSON::toString(right, true);
}
} // namespace

juce::String CliAudioSuiteOperationRegistry::canonicalOperationId(
    const juce::String& idOrAlias)
{
    if (const auto* operation = CliColdManifest::findOperation(idOrAlias))
        return operation->id;
    return {};
}

bool CliAudioSuiteOperationRegistry::validateCanonicalParameters(
    const juce::String& operationId,
    const juce::var& parameters,
    juce::var& canonicalParameters,
    juce::String& error)
{
    const auto canonicalId = canonicalOperationId(operationId);
    if (canonicalId == CliColdManifest::referenceGain().id)
    {
        double gainDb = 0.0;
        return parseReferenceGain(parameters, gainDb, canonicalParameters, error)
            && canonicalValuesMatch(parameters, canonicalParameters);
    }
    if (canonicalId == CliColdManifest::parametricEq().id)
    {
        ParametricEqParameters parsed;
        return parseEq(parameters, parsed, canonicalParameters, error)
            && canonicalValuesMatch(parameters, canonicalParameters);
    }
    if (canonicalId == CliColdManifest::truePeakLimiter().id)
    {
        TruePeakLimiterParameters parsed;
        return parseLimiter(parameters, parsed, canonicalParameters, error)
            && canonicalValuesMatch(parameters, canonicalParameters);
    }
    error = "Operation is not present in the cold AudioSuite registry";
    return false;
}

CliAudioSuiteAnalyzeResult CliAudioSuiteOperationRegistry::analyze(
    const juce::String& operationId,
    SingleTrackDocument::StatePtr state,
    int64_t documentGeneration,
    const AudioSelectionRequest& selection,
    const juce::var& parameters)
{
    CliAudioSuiteAnalyzeResult result;
    const auto* declaration = CliColdManifest::findOperation(operationId);
    if (declaration == nullptr)
    {
        result.errorMessage = "Operation is not present in the cold AudioSuite registry";
        return result;
    }
    result.operationId = declaration->id;
    result.operationVersion = declaration->operationVersion;
    result.algorithmVersion = declaration->algorithmVersion;

    if (declaration->id == CliColdManifest::referenceGain().id)
    {
        double gainDb = 0.0;
        if (!parseReferenceGain(parameters, gainDb, result.canonicalParameters,
                                result.errorMessage))
            return result;
        const auto descriptor = AudioSuiteReferenceProcessor::describe();
        if (descriptor.processorId != declaration->id
            || descriptor.processorVersion != declaration->operationVersion)
        {
            result.errorMessage = "Cold manifest and Reference Gain processor disagree";
            return result;
        }
        auto processor = descriptor.create();
        const auto analyzed = processor->analyze(
            state, documentGeneration, selection,
            static_cast<float>(std::pow(10.0, gainDb / 20.0)));
        if (!analyzed.succeeded())
        {
            result.errorMessage = analyzed.errorMessage;
            return result;
        }
        result.canonicalParameters = canonicalReferenceGain(
            gainDb, static_cast<double>(analyzed.plan->getLinearGain()));
        result.selection = analyzed.plan->getSelection();
        result.plan = analyzed.plan;
        return result;
    }

    if (declaration->id == CliColdManifest::parametricEq().id)
    {
        ParametricEqParameters parsed;
        if (!parseEq(parameters, parsed, result.canonicalParameters,
                     result.errorMessage))
            return result;
        const auto descriptor = ParametricEqProcessor::describe();
        if (descriptor.processorId != declaration->id
            || descriptor.processorVersion != declaration->operationVersion)
        {
            result.errorMessage = "Cold manifest and Parametric EQ processor disagree";
            return result;
        }
        auto processor = descriptor.create();
        const auto analyzed = processor->analyze(
            state, documentGeneration, selection, parsed);
        if (!analyzed.succeeded())
        {
            result.errorMessage = analyzed.errorMessage;
            return result;
        }
        result.canonicalParameters = canonicalEq(analyzed.plan->getParameters());
        result.selection = analyzed.plan->getAudioSuitePlan()->getSelection();
        result.plan = analyzed.plan;
        return result;
    }

    TruePeakLimiterParameters parsed;
    if (!parseLimiter(parameters, parsed, result.canonicalParameters,
                      result.errorMessage))
        return result;
    const auto descriptor = TruePeakLimiterProcessor::describe();
    if (descriptor.processorId != declaration->id
        || descriptor.processorVersion != declaration->operationVersion)
    {
        result.errorMessage = "Cold manifest and True-Peak Limiter processor disagree";
        return result;
    }
    auto processor = descriptor.create();
    const auto analyzed = processor->analyze(
        state, documentGeneration, selection, parsed);
    if (!analyzed.succeeded())
    {
        result.errorMessage = analyzed.errorMessage;
        return result;
    }
    result.canonicalParameters = canonicalLimiter(analyzed.plan->getParameters());
    result.selection = analyzed.plan->getAudioSuitePlan()->getSelection();
    result.plan = analyzed.plan;
    return result;
}

AudioSuiteApplyResult CliAudioSuiteOperationRegistry::apply(
    const CliAudioSuiteAnalyzeResult& analyzed,
    SingleTrackDocument& document,
    int64_t currentDocumentGeneration,
    const AudioSuiteCancellationToken& cancellation)
{
    if (const auto* plan = std::get_if<AudioSuitePlanPtr>(&analyzed.plan))
    {
        auto processor = AudioSuiteReferenceProcessor::describe().create();
        return processor->apply(document, *plan, currentDocumentGeneration,
                                cancellation);
    }
    if (const auto* plan = std::get_if<ParametricEqPlanPtr>(&analyzed.plan))
    {
        auto processor = ParametricEqProcessor::describe().create();
        return processor->apply(document, *plan, currentDocumentGeneration,
                                cancellation);
    }
    if (const auto* plan = std::get_if<TruePeakLimiterPlanPtr>(&analyzed.plan))
    {
        auto processor = TruePeakLimiterProcessor::describe().create();
        return processor->apply(document, *plan, currentDocumentGeneration,
                                cancellation);
    }
    return {};
}

AudioSuitePlanPtr CliAudioSuiteOperationRegistry::basePlan(
    const CliAudioSuiteAnalyzeResult& analyzed)
{
    if (const auto* plan = std::get_if<AudioSuitePlanPtr>(&analyzed.plan))
        return *plan;
    if (const auto* plan = std::get_if<ParametricEqPlanPtr>(&analyzed.plan))
        return (*plan)->getAudioSuitePlan();
    if (const auto* plan = std::get_if<TruePeakLimiterPlanPtr>(&analyzed.plan))
        return (*plan)->getAudioSuitePlan();
    return {};
}

int CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() noexcept
{
    return AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics()
        + ParametricEqProcessor::getLiveInstanceCountForDiagnostics()
        + TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics();
}
} // namespace singleeditor
