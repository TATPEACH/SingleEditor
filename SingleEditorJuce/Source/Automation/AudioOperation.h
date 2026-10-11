#pragma once

#include "Automation/AutomationExitCode.h"

#include <juce_core/juce_core.h>

namespace singleeditor
{
enum class AudioOperationMode
{
    inspect,
    analyze,
    render
};

struct AudioOperationDescriptor
{
    juce::String id;
    int schemaVersion = 1;
    juce::String displayName;
    AudioOperationMode mode = AudioOperationMode::inspect;
    juce::var parameterSchema;
};

struct AudioOperationRequest
{
    juce::Array<juce::File> inputs;
    juce::var parameters;
};

struct AudioOperationResult
{
    AutomationExitCode exitCode = AutomationExitCode::success;
    juce::var payload;
    juce::String errorMessage;

    bool succeeded() const noexcept
    {
        return exitCode == AutomationExitCode::success;
    }
};

class AudioOperation
{
public:
    virtual ~AudioOperation() = default;
    virtual AudioOperationDescriptor descriptor() const = 0;
    virtual AudioOperationResult execute(const AudioOperationRequest&) const = 0;
};
} // namespace singleeditor
