#pragma once
#include <juce_core/juce_core.h>
#include <optional>
std::optional<int> tryDeviceProbeTestWorker(const juce::StringArray&);
void runDeviceDiagnosticProbeTests();
