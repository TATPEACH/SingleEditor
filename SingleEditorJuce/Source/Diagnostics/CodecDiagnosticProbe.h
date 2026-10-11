#pragma once
#include "DiagnosticSnapshot.h"
#include "Audio/BoundedDecodeProcess.h"

namespace singleeditor::diagnostics
{
// Explicit active probe. It never creates a Document, device, cache or GUI owner.
struct CodecProbeResult final : LoadedDiagnosticProvider
{
    ProviderEvidence evidence;
    bool succeeded = false, cancelled = false, temporaryRemoved = true;
    ProviderEvidence snapshot() const override { return evidence; }
};
class CodecDiagnosticProbe final
{
public:
    enum class Origin { standaloneCli, gui };
    static constexpr int windowFrames = 4096;
    static constexpr int64_t readBudget = 8 * 1024 * 1024;
    static media::BoundedDecodeProcess::Limits limits();
    static CodecProbeResult run(const juce::File& workerExecutable, const juce::File& source,
                                const juce::Uuid& operation, const std::atomic<bool>& cancelled,
                                media::BoundedDecodeProcess::Limits = limits(), Origin = Origin::standaloneCli);
    // Internal worker dispatch precedes ordinary CLI command construction.
    // This entry has the same fixed read/window limits and never emits audio.
    static std::optional<int> tryWorker(const juce::StringArray&);
};
}
