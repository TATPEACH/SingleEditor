#pragma once

#include "Audio/AmbisonicBinauralFilterBank.h"
#include "Audio/AmbisonicFieldAnalyzer.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/ChannelRouter.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace singleeditor
{
struct RoutingInspectionOptions
{
    int outputChannels = 0;
    juce::String sourcePresetId;
    bool includeLfe = false;
    juce::StringArray outputMappings;
    juce::Array<int> mutedSourceChannels;
    juce::Array<int> soloedSourceChannels;
    MonitorMode monitorMode = MonitorMode::autoFollowDevice;
    AmbisonicMonitorMode ambisonicMonitorMode = AmbisonicMonitorMode::speakerProjection;
    AmbisonicOrientation ambisonicOrientation;
    bool ambisonicOrientationSpecified = false;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> customDecodeArtifact;
    bool runtimeProbe = false;
    double probeStartSeconds = 0.0;
};

struct RuntimeRenderProbeChannel
{
    SpeakerRole role = SpeakerRole::unknown;
    float peakLinear = 0.0f;
    float rmsLinear = 0.0f;
};

struct RuntimeRenderProbeResult
{
    bool requested = false;
    bool succeeded = false;
    bool finite = true;
    double startSeconds = 0.0;
    int64_t startSample = 0;
    int sampleCount = 0;
    juce::String renderPath;
    int transportChannelCapacity = 0;
    AmbisonicFieldSnapshot ambisonicField;
    std::vector<RuntimeRenderProbeChannel> channels;
    juce::String errorMessage;
};

struct RoutingInspectionResult
{
    juce::File input;
    bool opened = false;
    bool matrixFinite = true;
    bool outputMappingValid = true;
    bool ambisonicStageGraphActive = false;
    bool ambisonicDecodeMatrixAvailable = false;
    bool ambisonicDecodeMatrixUsesCustomGeometry = false;
    bool ambisonicDecodeMatrixGeneratedOffline = false;
    juce::String ambisonicDecodeMatrixArtifactId;
    juce::String ambisonicDecodeMatrixCacheKey;
    juce::String ambisonicGeometryProfileId;
    juce::String ambisonicGeometryProfileHash;
    int64_t ambisonicGeometryProfileRevision = 0;
    bool ambisonicBinauralRendererActive = false;
    bool ambisonicBinauralSelfTestPassed = false;
    double ambisonicBinauralLeftEnergy = 0.0;
    double ambisonicBinauralRightEnergy = 0.0;
    std::shared_ptr<const AmbisonicBinauralFilterBank> ambisonicBinauralFilterBank;
    juce::String ambisonicBinauralError;
    AmbisonicMonitorMode ambisonicMonitorMode = AmbisonicMonitorMode::speakerProjection;
    AmbisonicOrientation ambisonicOrientation;
    ChannelLayoutState sourceLayout;
    ChannelLayoutState targetLayout;
    ChannelLayoutState deviceOutputLayout;
    ChannelPlaybackState playbackState;
    RoutingMatrix matrix;
    PhysicalOutputMatrix physicalOutputMatrix;
    std::vector<SilentRouteProbe> silentRouteProbes;
    RuntimeRenderProbeResult runtimeRenderProbe;
    std::vector<int> unexpectedUnroutedSourceChannels;
    juce::String errorMessage;

    bool succeeded() const noexcept;
    juce::var toJson() const;
};

class RoutingInspectionService final
{
public:
    static RoutingInspectionResult inspect(const juce::File& file,
                                           RoutingInspectionOptions options);
};
} // namespace singleeditor
