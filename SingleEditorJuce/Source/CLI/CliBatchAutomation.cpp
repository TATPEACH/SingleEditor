#include "CLI/CliBatchAutomation.h"

#include "Automation/AutomationPlanIO.h"
#include "Automation/FileIdentity.h"
#include "CLI/CliColdManifest.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <set>
#include <vector>

namespace singleeditor
{
namespace
{
struct BatchOutcome
{
    AutomationExitCode exitCode = AutomationExitCode::success;
    juce::var payload;
    juce::String errorCode;
    juce::String errorMessage;

    bool succeeded() const noexcept
    {
        return exitCode == AutomationExitCode::success;
    }
};

class BatchEventStream final
{
public:
    explicit BatchEventStream(juce::String commandToUse)
        : command(std::move(commandToUse))
    {
        append("started", false, {});
    }

    void progress(const juce::String& phase,
                  int itemIndex,
                  int itemCount,
                  const juce::String& itemId,
                  const juce::String& sourcePath)
    {
        auto* payload = new juce::DynamicObject();
        payload->setProperty("phase", phase);
        payload->setProperty("item_index", itemIndex);
        payload->setProperty("item_count", itemCount);
        payload->setProperty("item_id", itemId);
        payload->setProperty("file", sourcePath);
        payload->setProperty("fraction",
                             itemCount > 0
                                 ? static_cast<double>(itemIndex) / itemCount
                                 : 0.0);
        append("progress", false, juce::var(payload));
    }

    void itemResult(const juce::var& payload)
    {
        append("item-result", false, payload);
    }

    void itemError(const juce::var& payload)
    {
        append("item-error", false, payload);
    }

    void finish(const BatchOutcome& outcome)
    {
        if (hasTerminal)
            return;
        hasTerminal = true;
        auto* event = makeBase(outcome.succeeded() ? "result" : "error", true);
        event->setProperty("ok", outcome.succeeded());
        event->setProperty("exit_code", static_cast<int>(outcome.exitCode));
        if (!outcome.payload.isVoid())
            event->setProperty("payload", outcome.payload);
        if (!outcome.succeeded())
        {
            auto* error = new juce::DynamicObject();
            error->setProperty("code", outcome.errorCode);
            error->setProperty("message", outcome.errorMessage);
            event->setProperty("error", juce::var(error));
        }
        text << juce::JSON::toString(juce::var(event), true) << "\n";
    }

    juce::String takeText() const { return text; }

private:
    juce::DynamicObject* makeBase(const juce::String& type, bool terminal)
    {
        auto* event = new juce::DynamicObject();
        event->setProperty("schema", CliColdManifest::eventSchema);
        event->setProperty("protocol_version", CliColdManifest::eventProtocolVersion);
        event->setProperty("sequence", ++sequence);
        event->setProperty("type", type);
        event->setProperty("terminal", terminal);
        event->setProperty("command", command);
        event->setProperty("operation", juce::var());
        return event;
    }

    void append(const juce::String& type, bool terminal, const juce::var& payload)
    {
        auto* event = makeBase(type, terminal);
        if (!payload.isVoid())
            event->setProperty("payload", payload);
        text << juce::JSON::toString(juce::var(event), true) << "\n";
    }

    juce::String command;
    juce::String text;
    int sequence = 0;
    bool hasTerminal = false;
};

struct BatchPlanArguments
{
    juce::File output;
    std::vector<std::pair<juce::String, juce::File>> items;
    bool jsonl = false;
};

struct BatchApplyArguments
{
    juce::File batchPlan;
    juce::File evidence;
    juce::File cancelFile;
    bool resume = false;
    bool allowInPlace = false;
    bool allowOverwriteExisting = false;
    bool jsonl = false;
};

struct BatchPlanItem
{
    juce::String itemId;
    juce::String planSha256;
    juce::var plan;
    juce::String sourcePath;
    juce::String destinationPath;
    juce::String destinationMode;
    juce::String operationId;
};

struct BatchPlan
{
    std::vector<BatchPlanItem> items;
    juce::String batchSha256;
};

struct EvidenceItem
{
    juce::String itemId;
    juce::String planSha256;
    juce::String status = "pending";
    std::optional<int> exitCode;
    juce::var result;
    juce::var error;
};

struct BatchEvidence
{
    juce::String batchSha256;
    juce::String status = "running";
    std::vector<EvidenceItem> items;
};

juce::String sha256For(const juce::String& text)
{
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}

bool samePath(const juce::String& left, const juce::String& right) noexcept
{
#if JUCE_WINDOWS
    return left.equalsIgnoreCase(right);
#else
    return left == right;
#endif
}

juce::String pathKey(const juce::String& path)
{
#if JUCE_WINDOWS
    return path.toLowerCase();
#else
    return path;
#endif
}

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

bool takeValue(const juce::StringArray& arguments,
               int& index,
               juce::String& value,
               juce::String& error)
{
    if (index + 1 >= arguments.size())
    {
        error = arguments[index] + " requires a value";
        return false;
    }
    value = arguments[++index];
    return true;
}

bool validItemId(const juce::String& value)
{
    return value.isNotEmpty() && value.length() <= 128
        && value.containsOnly(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
}

bool parseBatchPlanArguments(const juce::StringArray& arguments,
                             BatchPlanArguments& parsed,
                             juce::String& error)
{
    for (int index = 1; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index];
        juce::String value;
        if (argument == "--jsonl")
            parsed.jsonl = true;
        else if (argument == "--output")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.output = juce::File(value);
        }
        else if (argument == "--item")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            const auto itemId = value;
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.items.emplace_back(itemId, juce::File(value));
        }
        else
        {
            error = "Unknown batch-plan argument: " + argument;
            return false;
        }
    }
    if (!parsed.jsonl)
        error = "M3 machine commands require --jsonl";
    else if (parsed.output == juce::File())
        error = "batch-plan requires --output";
    else if (parsed.items.empty())
        error = "batch-plan requires at least one --item <id> <single-plan>";
    else if (parsed.items.size() > CliColdManifest::maximumBatchItems)
        error = "batch-plan exceeds the advertised maximum item count";
    return error.isEmpty();
}

bool parseBatchApplyArguments(const juce::StringArray& arguments,
                              BatchApplyArguments& parsed,
                              juce::String& error)
{
    for (int index = 1; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index];
        juce::String value;
        if (argument == "--jsonl") parsed.jsonl = true;
        else if (argument == "--resume") parsed.resume = true;
        else if (argument == "--allow-in-place") parsed.allowInPlace = true;
        else if (argument == "--allow-overwrite-existing")
            parsed.allowOverwriteExisting = true;
        else if (argument == "--batch-plan")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.batchPlan = juce::File(value);
        }
        else if (argument == "--evidence")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.evidence = juce::File(value);
        }
        else if (argument == "--cancel-file")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.cancelFile = juce::File(value);
        }
        else
        {
            error = "Unknown batch-apply argument: " + argument;
            return false;
        }
    }
    if (!parsed.jsonl)
        error = "M3 machine commands require --jsonl";
    else if (parsed.batchPlan == juce::File())
        error = "batch-apply requires --batch-plan";
    else if (parsed.evidence == juce::File())
        error = "batch-apply requires --evidence";
    else if (samePath(parsed.batchPlan.getFullPathName(),
                      parsed.evidence.getFullPathName()))
        error = "Batch Plan and evidence paths must be distinct";
    return error.isEmpty();
}

juce::var buildBatchPlan(const BatchPlan& plan, bool includeHash)
{
    juce::Array<juce::var> items;
    for (const auto& source : plan.items)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("item_id", source.itemId);
        item->setProperty("plan_sha256", source.planSha256);
        item->setProperty("plan", source.plan);
        items.add(juce::var(item));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("schema", CliColdManifest::batchPlanSchema);
    root->setProperty("schema_version", CliColdManifest::batchPlanSchemaVersion);
    root->setProperty("event_protocol", CliColdManifest::eventProtocolVersion);
    root->setProperty("concurrency_limit", CliColdManifest::maximumBatchConcurrency);
    root->setProperty("items", juce::var(items));
    if (includeHash)
        root->setProperty("batch_sha256", plan.batchSha256);
    return juce::var(root);
}

juce::String batchHash(const BatchPlan& plan)
{
    return sha256For(juce::JSON::toString(buildBatchPlan(plan, false), true));
}

bool populatePlanItem(const juce::String& itemId,
                      const juce::var& planValue,
                      BatchPlanItem& item,
                      juce::String& error)
{
    juce::var canonical;
    if (!inspectM3SingleFilePlanValue(planValue, canonical, error))
        return false;
    item.itemId = itemId;
    item.plan = canonical;
    item.planSha256 = canonical["plan_sha256"].toString();
    item.sourcePath = canonical["source"]["identity"]["canonical_path"].toString();
    item.destinationPath = canonical["destination"]["canonical_path"].toString();
    item.destinationMode = canonical["destination"]["mode"].toString();
    item.operationId = canonical["operation"]["id"].toString();
    if (!validItemId(item.itemId) || item.planSha256.isEmpty()
        || item.sourcePath.isEmpty() || item.destinationPath.isEmpty()
        || item.operationId.isEmpty())
    {
        error = "Batch item identity or imported Plan summary is invalid";
        return false;
    }
    return true;
}

bool validateIndependentItems(const BatchPlan& plan, juce::String& error)
{
    std::set<juce::String> ids;
    std::set<juce::String> sources;
    std::set<juce::String> destinations;
    for (const auto& item : plan.items)
    {
        const auto source = pathKey(item.sourcePath);
        const auto destination = pathKey(item.destinationPath);
        if (!ids.insert(item.itemId).second)
        {
            error = "Batch item IDs must be unique";
            return false;
        }
        if (!sources.insert(source).second)
        {
            error = "Batch source files must be unique independent items";
            return false;
        }
        if (!destinations.insert(destination).second)
        {
            error = "Batch destination files must be unique";
            return false;
        }
    }
    for (const auto& item : plan.items)
    {
        const auto destination = pathKey(item.destinationPath);
        const auto ownSource = pathKey(item.sourcePath);
        if (sources.count(destination) != 0
            && !(item.destinationMode == "in_place" && destination == ownSource))
        {
            error = "A Batch destination may not alias another item source";
            return false;
        }
    }
    return true;
}

bool parseBatchPlan(const juce::var& value,
                    BatchPlan& plan,
                    juce::String& error)
{
    const auto* root = value.getDynamicObject();
    if (root == nullptr
        || !hasExactProperties(*root,
                               { "schema", "schema_version", "event_protocol",
                                 "concurrency_limit", "items", "batch_sha256" })
        || root->getProperty("schema").toString() != CliColdManifest::batchPlanSchema
        || root->getProperty("schema_version").toString()
               != CliColdManifest::batchPlanSchemaVersion
        || root->getProperty("event_protocol").toString()
               != CliColdManifest::eventProtocolVersion
        || static_cast<int>(root->getProperty("concurrency_limit"))
               != CliColdManifest::maximumBatchConcurrency)
    {
        error = "Batch Plan schema/version/concurrency is incompatible";
        return false;
    }
    const auto* items = root->getProperty("items").getArray();
    if (items == nullptr || items->isEmpty()
        || items->size() > CliColdManifest::maximumBatchItems)
    {
        error = "Batch Plan item count is outside the advertised bound";
        return false;
    }
    for (const auto& valueForItem : *items)
    {
        const auto* object = valueForItem.getDynamicObject();
        if (object == nullptr
            || !hasExactProperties(*object, { "item_id", "plan_sha256", "plan" }))
        {
            error = "Batch Plan item structure is invalid";
            return false;
        }
        BatchPlanItem item;
        if (!populatePlanItem(object->getProperty("item_id").toString(),
                              object->getProperty("plan"), item, error)
            || item.planSha256 != object->getProperty("plan_sha256").toString())
        {
            if (error.isEmpty()) error = "Batch item Plan hash is invalid";
            return false;
        }
        plan.items.push_back(std::move(item));
    }
    if (!validateIndependentItems(plan, error))
        return false;
    plan.batchSha256 = root->getProperty("batch_sha256").toString();
    if (plan.batchSha256 != batchHash(plan))
    {
        error = "Batch Plan hash does not match its canonical content";
        return false;
    }
    return true;
}

juce::var buildEvidence(const BatchEvidence& evidence)
{
    juce::Array<juce::var> items;
    for (const auto& source : evidence.items)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("item_id", source.itemId);
        item->setProperty("plan_sha256", source.planSha256);
        item->setProperty("status", source.status);
        item->setProperty("exit_code",
                          source.exitCode.has_value() ? juce::var(*source.exitCode)
                                                     : juce::var());
        item->setProperty("result", source.result);
        item->setProperty("error", source.error);
        items.add(juce::var(item));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("schema", CliColdManifest::batchEvidenceSchema);
    root->setProperty("schema_version", CliColdManifest::batchEvidenceSchemaVersion);
    root->setProperty("batch_sha256", evidence.batchSha256);
    root->setProperty("status", evidence.status);
    root->setProperty("items", juce::var(items));
    return juce::var(root);
}

bool writeEvidence(const juce::File& file,
                   const BatchEvidence& evidence,
                   juce::String& error)
{
    return writeAutomationPlanAtomically(file, buildEvidence(evidence), error);
}

bool parseEvidence(const juce::var& value,
                   const BatchPlan& plan,
                   BatchEvidence& evidence,
                   juce::String& error)
{
    const auto* root = value.getDynamicObject();
    if (root == nullptr
        || !hasExactProperties(*root,
                               { "schema", "schema_version", "batch_sha256",
                                 "status", "items" })
        || root->getProperty("schema").toString()
               != CliColdManifest::batchEvidenceSchema
        || root->getProperty("schema_version").toString()
               != CliColdManifest::batchEvidenceSchemaVersion
        || root->getProperty("batch_sha256").toString() != plan.batchSha256)
    {
        error = "Resume evidence schema or Batch identity is incompatible";
        return false;
    }
    const auto status = root->getProperty("status").toString();
    if (!juce::StringArray { "running", "completed", "partial", "cancelled" }
             .contains(status))
    {
        error = "Resume evidence status is invalid";
        return false;
    }
    const auto* items = root->getProperty("items").getArray();
    if (items == nullptr || items->size() != static_cast<int>(plan.items.size()))
    {
        error = "Resume evidence item set does not match the Batch Plan";
        return false;
    }
    evidence.batchSha256 = plan.batchSha256;
    evidence.status = status;
    for (int index = 0; index < items->size(); ++index)
    {
        const auto* object = items->getReference(index).getDynamicObject();
        if (object == nullptr
            || !hasExactProperties(*object,
                                   { "item_id", "plan_sha256", "status",
                                     "exit_code", "result", "error" }))
        {
            error = "Resume evidence item structure is invalid";
            return false;
        }
        EvidenceItem item;
        item.itemId = object->getProperty("item_id").toString();
        item.planSha256 = object->getProperty("plan_sha256").toString();
        item.status = object->getProperty("status").toString();
        const auto exitCode = object->getProperty("exit_code");
        if (!exitCode.isVoid()) item.exitCode = static_cast<int>(exitCode);
        item.result = object->getProperty("result");
        item.error = object->getProperty("error");
        if (item.itemId != plan.items[static_cast<std::size_t>(index)].itemId
            || item.planSha256
                   != plan.items[static_cast<std::size_t>(index)].planSha256
            || !juce::StringArray { "pending", "succeeded", "failed" }
                    .contains(item.status))
        {
            error = "Resume evidence item identity/status is incompatible";
            return false;
        }
        if ((item.status == "pending"
             && (item.exitCode.has_value() || !item.result.isVoid()
                 || !item.error.isVoid()))
            || (item.status == "succeeded"
                && (!item.exitCode.has_value()
                    || *item.exitCode != static_cast<int>(AutomationExitCode::success)
                    || !item.result.isObject() || !item.error.isVoid()))
            || (item.status == "failed"
                && (!item.exitCode.has_value()
                    || *item.exitCode == static_cast<int>(AutomationExitCode::success)
                    || !item.result.isVoid() || !item.error.isObject())))
        {
            error = "Resume evidence item outcome fields are inconsistent";
            return false;
        }
        evidence.items.push_back(std::move(item));
    }
    const auto pending = std::count_if(evidence.items.begin(), evidence.items.end(),
                                       [](const auto& item)
                                       {
                                           return item.status == "pending";
                                       });
    const auto succeeded = std::count_if(evidence.items.begin(), evidence.items.end(),
                                         [](const auto& item)
                                         {
                                             return item.status == "succeeded";
                                         });
    const auto failed = std::count_if(evidence.items.begin(), evidence.items.end(),
                                      [](const auto& item)
                                      {
                                          return item.status == "failed";
                                      });
    if ((evidence.status == "completed"
         && succeeded != static_cast<int64_t>(evidence.items.size()))
        || (evidence.status == "partial" && (failed == 0 || pending != 0))
        || (evidence.status == "cancelled" && pending == 0))
    {
        error = "Resume evidence aggregate status is inconsistent with its items";
        return false;
    }
    return true;
}

bool appliedSelectionMatchesPlan(const juce::var& appliedValue,
                                 const juce::var& planValue)
{
    const auto* applied = appliedValue.getDynamicObject();
    const auto* planned = planValue.getDynamicObject();
    if (applied == nullptr || planned == nullptr
        || !hasExactProperties(*applied,
                               { "start_sample", "end_sample",
                                 "source_channels" }))
        return false;
    const auto* appliedChannels = applied->getProperty("source_channels").getArray();
    const auto* plannedChannels = planned->getProperty("channels").getArray();
    if (appliedChannels == nullptr || plannedChannels == nullptr
        || appliedChannels->size() != plannedChannels->size()
        || static_cast<int64_t>(applied->getProperty("start_sample"))
               != static_cast<int64_t>(planned->getProperty("start_sample"))
        || static_cast<int64_t>(applied->getProperty("end_sample"))
               != static_cast<int64_t>(planned->getProperty("end_sample")))
        return false;
    for (int index = 0; index < appliedChannels->size(); ++index)
    {
        const auto* plannedChannel = plannedChannels->getReference(index)
                                         .getDynamicObject();
        if (plannedChannel == nullptr
            || static_cast<int>(appliedChannels->getReference(index))
                   != static_cast<int>(plannedChannel->getProperty("source_index")))
            return false;
    }
    return true;
}

bool validDocumentIdentityReceipt(const juce::var& value)
{
    const auto* object = value.getDynamicObject();
    const auto validUuid = [] (const juce::var& id)
    {
        const auto text = id.toString();
        return id.isString() && text.length() == 32 && text.containsOnly("0123456789abcdef")
            && !juce::Uuid(text).isNull();
    };
    const auto positiveInteger = [] (const juce::var& number)
    {
        return (number.isInt() || number.isInt64()) && static_cast<juce::int64>(number) > 0;
    };
    return object != nullptr
        && hasExactProperties(*object, { "version", "lineage", "runtime_instance", "generation", "event_sequence" })
        && value["version"].isInt() && static_cast<int>(value["version"]) == 1
        && validUuid(value["lineage"]) && validUuid(value["runtime_instance"])
        && positiveInteger(value["generation"]) && positiveInteger(value["event_sequence"]);
}

bool validateCompletedEvidenceOutputs(const BatchEvidence& evidence,
                                      const BatchPlan& plan,
                                      juce::String& error)
{
    for (std::size_t index = 0; index < evidence.items.size(); ++index)
    {
        const auto& item = evidence.items[index];
        if (item.status != "succeeded")
            continue;
        const auto* result = item.result.getDynamicObject();
        const auto output = juce::File(item.result["output"].toString());
        FileIdentity expected;
        if (result == nullptr
            || !(hasExactProperties(*result,
                                   { "operation", "plan_sha256", "output",
                                     "output_identity", "write_performed",
                                     "rendered_samples", "rendered_blocks",
                                     "selection" })
                 || (hasExactProperties(*result,
                                    { "operation", "plan_sha256", "output",
                                      "output_identity", "write_performed",
                                      "rendered_samples", "rendered_blocks", "selection", "document_identity" })
                     && validDocumentIdentityReceipt(item.result["document_identity"])))
            || item.result["operation"].toString() != plan.items[index].operationId
            || item.result["plan_sha256"].toString() != item.planSha256
            || !samePath(output.getFullPathName(), plan.items[index].destinationPath)
            || !item.result["write_performed"].isBool()
            || !static_cast<bool>(item.result["write_performed"])
            || !appliedSelectionMatchesPlan(
                   item.result["selection"], plan.items[index].plan["selection"])
            || !output.existsAsFile()
            || !FileIdentity::fromJson(item.result["output_identity"], expected, error)
            || !expected.matches(FileIdentity::capture(output)))
        {
            if (error.isEmpty())
                error = "A previously completed Batch output changed before resume";
            return false;
        }
    }
    return true;
}

juce::var terminalEvent(const juce::String& jsonLines)
{
    juce::StringArray lines;
    lines.addLines(jsonLines);
    for (int index = lines.size(); --index >= 0;)
    {
        const auto value = juce::JSON::parse(lines[index]);
        if (value.isObject() && static_cast<bool>(value["terminal"]))
            return value;
    }
    return {};
}

juce::var itemEventPayload(const BatchPlanItem& planItem,
                           const EvidenceItem& evidenceItem)
{
    auto* payload = new juce::DynamicObject();
    payload->setProperty("item_id", planItem.itemId);
    payload->setProperty("plan_sha256", planItem.planSha256);
    payload->setProperty("operation", planItem.operationId);
    payload->setProperty("file", planItem.sourcePath);
    payload->setProperty("status", evidenceItem.status);
    payload->setProperty("exit_code",
                         evidenceItem.exitCode.has_value()
                             ? juce::var(*evidenceItem.exitCode) : juce::var());
    payload->setProperty("result", evidenceItem.result);
    payload->setProperty("error", evidenceItem.error);
    return juce::var(payload);
}

BatchOutcome createBatchPlan(const BatchPlanArguments& arguments)
{
    BatchOutcome outcome;
    if (!arguments.output.getParentDirectory().isDirectory()
        || arguments.output.exists())
    {
        outcome.exitCode = AutomationExitCode::conflict;
        outcome.errorCode = "BATCH_PLAN_OUTPUT_UNAVAILABLE";
        outcome.errorMessage = "Batch Plan output must be a new file in an existing directory";
        return outcome;
    }
    BatchPlan plan;
    for (const auto& source : arguments.items)
    {
        if (!source.second.existsAsFile() || source.second.getSize() > 16 * 1024 * 1024)
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "SINGLE_PLAN_READ_FAILED";
            outcome.errorMessage = "Imported single-file Plan is missing or too large";
            return outcome;
        }
        const auto value = juce::JSON::parse(source.second.loadFileAsString());
        BatchPlanItem item;
        juce::String error;
        if (!populatePlanItem(source.first, value, item, error))
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "SINGLE_PLAN_INVALID";
            outcome.errorMessage = error;
            return outcome;
        }
        if (samePath(arguments.output.getFullPathName(), item.sourcePath)
            || samePath(arguments.output.getFullPathName(), item.destinationPath))
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "BATCH_PLAN_PATH_ALIAS";
            outcome.errorMessage = "Batch Plan path cannot alias item source/destination";
            return outcome;
        }
        plan.items.push_back(std::move(item));
    }
    juce::String validationError;
    if (!validateIndependentItems(plan, validationError))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "BATCH_ITEMS_NOT_INDEPENDENT";
        outcome.errorMessage = validationError;
        return outcome;
    }
    plan.batchSha256 = batchHash(plan);
    juce::String writeError;
    if (!writeAutomationPlanAtomically(arguments.output,
                                       buildBatchPlan(plan, true), writeError))
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "BATCH_PLAN_WRITE_FAILED";
        outcome.errorMessage = writeError;
        return outcome;
    }
    auto* payload = new juce::DynamicObject();
    payload->setProperty("batch_plan", arguments.output.getFullPathName());
    payload->setProperty("batch_sha256", plan.batchSha256);
    payload->setProperty("item_count", static_cast<int>(plan.items.size()));
    payload->setProperty("concurrency_limit", CliColdManifest::maximumBatchConcurrency);
    payload->setProperty("write_performed", true);
    outcome.payload = juce::var(payload);
    return outcome;
}

BatchOutcome applyBatch(const BatchApplyArguments& arguments,
                        BatchEventStream& events)
{
    BatchOutcome outcome;
    if (!arguments.batchPlan.existsAsFile()
        || arguments.batchPlan.getSize() > 16 * 1024 * 1024
        || !arguments.evidence.getParentDirectory().isDirectory())
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "BATCH_INPUT_INVALID";
        outcome.errorMessage = "Batch Plan/evidence paths are unavailable";
        return outcome;
    }
    BatchPlan plan;
    juce::String planError;
    if (!parseBatchPlan(juce::JSON::parse(arguments.batchPlan.loadFileAsString()),
                        plan, planError))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "BATCH_PLAN_INVALID";
        outcome.errorMessage = planError;
        return outcome;
    }
    if (arguments.cancelFile != juce::File()
        && (samePath(arguments.cancelFile.getFullPathName(),
                     arguments.batchPlan.getFullPathName())
            || samePath(arguments.cancelFile.getFullPathName(),
                        arguments.evidence.getFullPathName())))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "CANCEL_PATH_ALIAS";
        outcome.errorMessage = "Cancel path cannot alias the Batch Plan/evidence path";
        return outcome;
    }
    for (const auto& item : plan.items)
    {
        if (samePath(arguments.evidence.getFullPathName(), item.sourcePath)
            || samePath(arguments.evidence.getFullPathName(), item.destinationPath))
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "EVIDENCE_PATH_ALIAS";
            outcome.errorMessage = "Evidence path cannot alias an item source/destination";
            return outcome;
        }
        if (arguments.cancelFile != juce::File()
            && (samePath(arguments.cancelFile.getFullPathName(), item.sourcePath)
                || samePath(arguments.cancelFile.getFullPathName(),
                            item.destinationPath)))
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "CANCEL_PATH_ALIAS";
            outcome.errorMessage = "Cancel path cannot alias an item source/destination";
            return outcome;
        }
        if (item.destinationMode == "in_place" && !arguments.allowInPlace)
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "IN_PLACE_NOT_AUTHORIZED";
            outcome.errorMessage = "Batch contains in_place Plans and requires --allow-in-place";
            return outcome;
        }
        if (item.destinationMode == "overwrite_existing"
            && !arguments.allowOverwriteExisting)
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "OVERWRITE_NOT_AUTHORIZED";
            outcome.errorMessage = "Batch contains overwrite Plans and requires --allow-overwrite-existing";
            return outcome;
        }
    }

    BatchEvidence evidence;
    if (arguments.resume)
    {
        juce::var evidenceValue;
        juce::String evidenceError;
        if (!readAutomationPlan(arguments.evidence, evidenceValue, evidenceError)
            || !parseEvidence(evidenceValue, plan, evidence, evidenceError)
            || !validateCompletedEvidenceOutputs(evidence, plan, evidenceError))
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "RESUME_EVIDENCE_INVALID";
            outcome.errorMessage = evidenceError;
            return outcome;
        }
        evidence.status = "running";
        for (auto& item : evidence.items)
            if (item.status != "succeeded") item = { item.itemId, item.planSha256 };
    }
    else
    {
        if (arguments.evidence.exists())
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "EVIDENCE_EXISTS";
            outcome.errorMessage = "Fresh batch-apply will not overwrite existing evidence";
            return outcome;
        }
        evidence.batchSha256 = plan.batchSha256;
        for (const auto& item : plan.items)
            evidence.items.push_back({ item.itemId, item.planSha256 });
    }
    juce::String evidenceError;
    if (!writeEvidence(arguments.evidence, evidence, evidenceError))
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "EVIDENCE_WRITE_FAILED";
        outcome.errorMessage = evidenceError;
        return outcome;
    }

    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
    for (std::size_t index = 0; index < plan.items.size(); ++index)
    {
        auto& evidenceItem = evidence.items[index];
        const auto& planItem = plan.items[index];
        if (evidenceItem.status == "succeeded")
        {
            ++succeeded;
            ++skipped;
            events.progress("resume-skip", static_cast<int>(index + 1),
                            static_cast<int>(plan.items.size()),
                            planItem.itemId, planItem.sourcePath);
            continue;
        }
        if (arguments.cancelFile != juce::File()
            && arguments.cancelFile.exists())
        {
            evidence.status = "cancelled";
            if (!writeEvidence(arguments.evidence, evidence, evidenceError))
            {
                outcome.exitCode = AutomationExitCode::executionFailed;
                outcome.errorCode = "EVIDENCE_WRITE_FAILED";
                outcome.errorMessage = evidenceError;
                return outcome;
            }
            auto* payload = new juce::DynamicObject();
            payload->setProperty("batch_sha256", plan.batchSha256);
            payload->setProperty("evidence", arguments.evidence.getFullPathName());
            payload->setProperty("succeeded", succeeded);
            payload->setProperty("failed", failed);
            payload->setProperty("remaining",
                                 static_cast<int>(plan.items.size() - index));
            outcome.payload = juce::var(payload);
            outcome.exitCode = AutomationExitCode::cancelled;
            outcome.errorCode = "CANCELLED";
            outcome.errorMessage = "Batch cancelled at a file boundary";
            return outcome;
        }

        events.progress("item-started", static_cast<int>(index),
                        static_cast<int>(plan.items.size()),
                        planItem.itemId, planItem.sourcePath);
        const auto temporaryPlan = arguments.evidence.getParentDirectory()
            .getNonexistentChildFile(".singleeditor-batch-" + planItem.itemId,
                                     ".json", false);
        juce::String temporaryError;
        if (!writeAutomationPlanAtomically(temporaryPlan, planItem.plan,
                                           temporaryError))
        {
            evidenceItem.status = "failed";
            evidenceItem.exitCode = static_cast<int>(AutomationExitCode::executionFailed);
            auto* error = new juce::DynamicObject();
            error->setProperty("code", "TEMP_PLAN_WRITE_FAILED");
            error->setProperty("message", temporaryError);
            evidenceItem.error = juce::var(error);
        }
        else
        {
            juce::StringArray applyArguments {
                "apply", "--plan", temporaryPlan.getFullPathName(), "--jsonl"
            };
            if (planItem.destinationMode == "in_place")
                applyArguments.add("--allow-in-place");
            else if (planItem.destinationMode == "overwrite_existing")
                applyArguments.add("--allow-overwrite-existing");
            const auto execution = tryExecuteM3SingleFileCli(applyArguments);
            temporaryPlan.deleteFile();
            const auto terminal = execution.has_value()
                                    ? terminalEvent(execution->stdoutText) : juce::var();
            if (!execution.has_value() || !terminal.isObject())
            {
                evidenceItem.status = "failed";
                evidenceItem.exitCode = static_cast<int>(AutomationExitCode::internalError);
                auto* error = new juce::DynamicObject();
                error->setProperty("code", "SINGLE_FILE_PROTOCOL_ERROR");
                error->setProperty("message", "Single-file Apply returned no terminal event");
                evidenceItem.error = juce::var(error);
            }
            else
            {
                evidenceItem.exitCode = static_cast<int>(execution->exitCode);
                if (execution->exitCode == AutomationExitCode::success)
                {
                    evidenceItem.status = "succeeded";
                    evidenceItem.result = terminal["payload"];
                    evidenceItem.error = juce::var();
                }
                else
                {
                    evidenceItem.status = "failed";
                    evidenceItem.result = juce::var();
                    evidenceItem.error = terminal["error"];
                }
            }
        }

        if (evidenceItem.status == "succeeded")
        {
            ++succeeded;
            events.itemResult(itemEventPayload(planItem, evidenceItem));
        }
        else
        {
            ++failed;
            events.itemError(itemEventPayload(planItem, evidenceItem));
        }
        if (!writeEvidence(arguments.evidence, evidence, evidenceError))
        {
            outcome.exitCode = AutomationExitCode::executionFailed;
            outcome.errorCode = "EVIDENCE_WRITE_FAILED_AFTER_ITEM";
            outcome.errorMessage = "An item finished but durable evidence could not be updated: "
                + evidenceError;
            return outcome;
        }
        events.progress("item-finished", static_cast<int>(index + 1),
                        static_cast<int>(plan.items.size()),
                        planItem.itemId, planItem.sourcePath);
    }

    evidence.status = failed == 0 ? "completed" : "partial";
    if (!writeEvidence(arguments.evidence, evidence, evidenceError))
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "EVIDENCE_FINALIZE_FAILED";
        outcome.errorMessage = evidenceError;
        return outcome;
    }
    auto* payload = new juce::DynamicObject();
    payload->setProperty("batch_sha256", plan.batchSha256);
    payload->setProperty("evidence", arguments.evidence.getFullPathName());
    payload->setProperty("status", evidence.status);
    payload->setProperty("item_count", static_cast<int>(plan.items.size()));
    payload->setProperty("succeeded", succeeded);
    payload->setProperty("failed", failed);
    payload->setProperty("resume_skipped", skipped);
    payload->setProperty("concurrency_limit", CliColdManifest::maximumBatchConcurrency);
    outcome.payload = juce::var(payload);
    if (failed != 0)
    {
        outcome.exitCode = AutomationExitCode::partialFailure;
        outcome.errorCode = "PARTIAL_FAILURE";
        outcome.errorMessage = "One or more independent Batch items failed";
    }
    return outcome;
}

CliSingleFileExecution finish(BatchEventStream& events, const BatchOutcome& outcome)
{
    events.finish(outcome);
    CliSingleFileExecution execution;
    execution.exitCode = outcome.exitCode;
    execution.stdoutText = events.takeText();
    return execution;
}
} // namespace

std::optional<CliSingleFileExecution> tryExecuteM3BatchCli(
    const juce::StringArray& arguments)
{
    if (arguments.isEmpty())
        return std::nullopt;
    if (arguments[0] == "batch-plan")
    {
        BatchEventStream events("batch-plan");
        BatchPlanArguments parsed;
        juce::String error;
        if (!parseBatchPlanArguments(arguments, parsed, error))
        {
            BatchOutcome outcome;
            outcome.exitCode = AutomationExitCode::invalidArguments;
            outcome.errorCode = "USAGE";
            outcome.errorMessage = error;
            return finish(events, outcome);
        }
        return finish(events, createBatchPlan(parsed));
    }
    if (arguments[0] == "batch-apply")
    {
        BatchEventStream events("batch-apply");
        BatchApplyArguments parsed;
        juce::String error;
        if (!parseBatchApplyArguments(arguments, parsed, error))
        {
            BatchOutcome outcome;
            outcome.exitCode = AutomationExitCode::invalidArguments;
            outcome.errorCode = "USAGE";
            outcome.errorMessage = error;
            return finish(events, outcome);
        }
        return finish(events, applyBatch(parsed, events));
    }
    return std::nullopt;
}
} // namespace singleeditor
