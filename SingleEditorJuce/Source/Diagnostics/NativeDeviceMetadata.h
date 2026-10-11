#pragma once
#include "DiagnosticSnapshot.h"

namespace singleeditor::diagnostics
{
// Explicit worker-only, read-only query of one copied, already-open endpoint ID.
// No endpoint enumeration, audio-client activation, format change, or fallback.
ProviderEvidence readNativeDeviceMetadata(const juce::String& endpoint, const std::atomic<bool>& cancelled);
}
