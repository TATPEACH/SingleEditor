#pragma once

#include <juce_core/juce_core.h>

// UI Debug Logging Utility
// Set SINGLE_EDITOR_UI_DEBUG to 1 to enable debug logs
#ifndef SINGLE_EDITOR_UI_DEBUG
#define SINGLE_EDITOR_UI_DEBUG 0
#endif

#if SINGLE_EDITOR_UI_DEBUG
#define UI_LOG(msg) ::singleeditor::uiDebugLog(__FILE__, __LINE__, msg)
#define UI_LOG_FMT(fmt, ...) ::singleeditor::uiDebugLogF(__FILE__, __LINE__, fmt, __VA_ARGS__)
#else
#define UI_LOG(msg) do {} while (false)
#define UI_LOG_FMT(fmt, ...) do {} while (false)
#endif

namespace singleeditor
{
inline void uiDebugLog(const char* file, int line, const juce::String& message)
{
    juce::String logMsg;
    logMsg << "[" << juce::Time::getCurrentTime().toString(true, true) << "] "
           << "[UI] "
           << juce::File(file).getFileName() << ":" << line << " "
           << message;
    juce::Logger::writeToLog(logMsg);
}

inline void uiDebugLogF(const char* file, int line, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    juce::String message;
    message.vprintf(fmt, args);
    va_end(args);
    uiDebugLog(file, line, message);
}
} // namespace singleeditor
