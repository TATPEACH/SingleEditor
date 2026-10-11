#pragma once
#include "DiagnosticBundle.h"
namespace singleeditor::diagnostics
{
// Explicit capture only; hashes the running binary and reads bounded OS identity.
BundleIdentity captureApplicationIdentity(const std::atomic<bool>& cancelled, const juce::String& distribution);
}
