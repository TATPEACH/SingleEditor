#include "Automation/AutomationPlanIO.h"

namespace singleeditor
{
bool readAutomationPlan(const juce::File& source,
                        juce::var& plan,
                        juce::String& errorMessage)
{
    if (source == juce::File() || !source.existsAsFile())
    {
        errorMessage = "Plan file does not exist";
        return false;
    }

    constexpr juce::int64 maximumPlanBytes = 16 * 1024 * 1024;
    if (source.getSize() <= 0 || source.getSize() > maximumPlanBytes)
    {
        errorMessage = "Plan file size is invalid";
        return false;
    }

    plan = juce::JSON::parse(source.loadFileAsString());
    if (!plan.isObject())
    {
        errorMessage = "Plan file is not valid JSON object";
        return false;
    }
    return true;
}

bool writeAutomationPlanAtomically(const juce::File& target,
                                   const juce::var& plan,
                                   juce::String& errorMessage)
{
    if (target == juce::File())
    {
        errorMessage = "Plan path is empty";
        return false;
    }

    const auto parent = target.getParentDirectory();
    if (!parent.isDirectory() && !parent.createDirectory())
    {
        errorMessage = "Could not create the plan directory";
        return false;
    }

    juce::TemporaryFile temporary(target);
    {
        juce::FileOutputStream output(temporary.getFile());
        if (!output.openedOk())
        {
            errorMessage = "Could not create the temporary plan file";
            return false;
        }

        output.writeText(juce::JSON::toString(plan, true), false, false, "\n");
        output.writeByte('\n');
        output.flush();
        if (output.getStatus().failed())
        {
            temporary.getFile().deleteFile();
            errorMessage = "Could not finish writing the plan file";
            return false;
        }
    }

    if (!temporary.overwriteTargetFileWithTemporary())
    {
        temporary.getFile().deleteFile();
        errorMessage = "Could not atomically replace the plan file";
        return false;
    }
    return true;
}
} // namespace singleeditor
