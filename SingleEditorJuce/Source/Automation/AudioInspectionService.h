#pragma once

#include "Document/SingleTrackDocument.h"

#include <vector>

namespace singleeditor
{
struct AudioChannelStats
{
    double peak = 0.0;
    double rms = 0.0;
    int64_t nonFiniteSamples = 0;
};

struct AudioInspectionOptions
{
    bool scanSamples = false;
    int blockSize = 65536;
};

struct AudioInspectionResult
{
    bool opened = false;
    bool sampleScanCompleted = false;
    SingleTrackDocument::FileInfo info;
    std::vector<AudioChannelStats> channels;
    int64_t samplesScannedPerChannel = 0;
    juce::String errorMessage;

    bool hasNonFiniteSamples() const noexcept;
    juce::var toJson() const;
};

class AudioInspectionService final
{
public:
    static AudioInspectionResult inspect(const juce::File& file,
                                         AudioInspectionOptions options = {});
};
} // namespace singleeditor
