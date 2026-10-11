#pragma once
#include "DiagnosticSnapshot.h"

namespace singleeditor::diagnostics
{
class ReproductionFiles final
{
public:
    static constexpr size_t maximumFiles = DiagnosticSnapshot::maximumReproductionFiles;
    static constexpr int64_t maximumTotalBytes = DiagnosticBundle::maximumArchiveBytes - 2 * 1024 * 1024;
    struct Result final { BundleStatus status; std::vector<BundleEntry> entries; };
    // Explicitly selected files only. Copies bytes without interpreting them.
    // All-or-nothing; no source write, directory scan or persistent copy.
    static Result capture(const std::vector<juce::File>&, const std::atomic<bool>& cancelled);
};
}
