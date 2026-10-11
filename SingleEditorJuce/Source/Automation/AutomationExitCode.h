#pragma once

namespace singleeditor
{
enum class AutomationExitCode : int
{
    success = 0,
    invalidArguments = 2,
    validationFailed = 3,
    unsupported = validationFailed,
    conflict = 4,
    staleInput = conflict,
    executionFailed = 5,
    processingFailed = executionFailed,
    cancelled = 6,
    partialFailure = cancelled,
    internalError = 7
};
} // namespace singleeditor
