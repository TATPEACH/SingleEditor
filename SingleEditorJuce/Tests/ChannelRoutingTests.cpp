#include "Audio/AmbisonicBinauralFilterBank.h"
#include "Audio/AmbisonicBinauralRenderer.h"
#include "Audio/AmbisonicFieldAnalyzer.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicSpeakerGeometry.h"
#include "Audio/AmbisonicStageGraph.h"
#include "Audio/ChannelLayout.h"
#include "Audio/ChannelRouter.h"
#include "Audio/OutputMeterAnalyzer.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Workspace/WorkspaceModuleLayout.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <utility>

namespace
{
using namespace singleeditor;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

float gainAt(const RoutingMatrix& matrix, int outputChannel, int sourceChannel)
{
    return matrix.gain(outputChannel, sourceChannel);
}

bool near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.0001f;
}

float maximumAbsoluteRowSum(const RoutingMatrix& matrix)
{
    auto maximum = 0.0f;
    for (int output = 0; output < matrix.outputChannels; ++output)
    {
        auto sum = 0.0f;
        for (int source = 0; source < matrix.sourceChannels; ++source)
            sum += std::abs(matrix.gain(output, source));
        maximum = juce::jmax(maximum, sum);
    }
    return maximum;
}

float firstOrderBasisDifference(int acn, AmbisonicOrientation orientation)
{
    constexpr std::array<std::pair<float, float>, 5> probes {
        std::pair { 0.0f, 0.0f },
        std::pair { 45.0f, 25.0f },
        std::pair { -70.0f, 15.0f },
        std::pair { 130.0f, -35.0f },
        std::pair { -155.0f, 50.0f }
    };
    auto difference = 0.0f;
    for (const auto& [azimuth, elevation] : probes)
    {
        std::array<float, 16> identity {};
        std::array<float, 16> rotated {};
        AmbisonicMonitorDecoder::evaluateSn3dBasis(1, azimuth, elevation, {}, identity);
        AmbisonicMonitorDecoder::evaluateSn3dBasis(1, azimuth, elevation, orientation, rotated);
        difference += std::abs(identity[static_cast<std::size_t>(acn)]
                             - rotated[static_cast<std::size_t>(acn)]);
    }
    return difference;
}

bool hasCandidate(const ChannelLayoutState& layout, const juce::String& presetId)
{
    for (const auto& candidate : layout.candidates)
    {
        if (candidate.id == presetId)
            return true;
    }

    return false;
}

bool writeTestWavFile(const juce::File& file,
                      int channels = 1,
                      const std::unordered_map<juce::String, juce::String>& metadata = {})
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto writerOptions = juce::AudioFormatWriterOptions()
                           .withSampleRate(48000.0)
                           .withNumChannels(channels)
                           .withBitsPerSample(16)
                           .withMetadataValues(metadata);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, writerOptions));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(channels, 4800);
    buffer.clear();
    for (int channel = 0; channel < channels; ++channel)
    {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(channel, sample, 0.5f);
    }

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

void runAmbiXMetadataDetectionTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorLogicTests")
                              .getNonexistentChildFile("ambix-metadata", {});
    testRoot.createDirectory();

    const auto ambixFile = testRoot.getChildFile("ambix-foa.wav");
    const std::unordered_map<juce::String, juce::String> ambixMetadata {
        { juce::WavAudioFormat::bwavCodingHistory,
          "A=PCM,F=48000,W=24,M=4,Rec Mode=AmbiX; Mic Position=Upright" },
        { juce::WavAudioFormat::bwavDescription,
          "Zoom H3-VR | zTRK1=W | zTRK2=Y | zTRK3=Z | zTRK4=X" }
    };
    require(writeTestWavFile(ambixFile, 4, ambixMetadata),
            "writes four-channel WAV with explicit AmbiX metadata");

    SingleTrackDocument ambixDocument;
    require(ambixDocument.loadFile(ambixFile), "loads AmbiX metadata fixture");
    const auto ambixLayout = ambixDocument.getState()->info.channelLayout;
    require(ambixLayout.isAmbisonic
                && ambixLayout.selectedPresetId == "foa-acn-sn3d"
                && ambixLayout.detectedFromMetadata,
            "explicit AmbiX metadata selects FOA ACN/SN3D without user interaction");
    require(AmbisonicMonitorDecoder::statusFor(ambixLayout)
                == AmbisonicMonitorDecodeStatus::ready
                && ambixDocument.setAmbisonicOrientation({ 45.0f, 0.0f, 0.0f }),
            "AmbiX metadata validates the lazy stage graph and accepts rotation immediately");
    require(ambixDocument.getState()->ambisonicBinauralFilterBank == nullptr,
            "AmbiX auto-detection does not load the optional HRTF resource");
    require(ambixDocument.setChannelLayoutPreset("quad-lrlsrs")
                && !ambixDocument.getState()->info.channelLayout.isAmbisonic,
            "an explicit user speaker-layout choice overrides AmbiX auto-detection");

    for (const auto [channels, presetId] : {
             std::pair { 9, juce::String("2oa-acn-sn3d") },
             std::pair { 16, juce::String("3oa-acn-sn3d") } })
    {
        const auto higherOrderFile = testRoot.getChildFile("ambix-" + juce::String(channels) + "ch.wav");
        require(writeTestWavFile(higherOrderFile, channels, ambixMetadata),
                "writes higher-order WAV with explicit AmbiX metadata");
        SingleTrackDocument higherOrderDocument;
        require(higherOrderDocument.loadFile(higherOrderFile)
                    && higherOrderDocument.getState()->info.channelLayout.selectedPresetId == presetId
                    && higherOrderDocument.getState()->info.channelLayout.detectedFromMetadata,
                "AmbiX metadata selects the matching ACN/SN3D higher-order preset");
    }

    const auto ordinaryFile = testRoot.getChildFile("ordinary-four-channel.wav");
    require(writeTestWavFile(ordinaryFile, 4), "writes unlabelled four-channel control fixture");
    SingleTrackDocument ordinaryDocument;
    require(ordinaryDocument.loadFile(ordinaryFile), "loads unlabelled four-channel control fixture");
    require(!ordinaryDocument.getState()->info.channelLayout.isAmbisonic
                && ordinaryDocument.getState()->info.channelLayout.selectedPresetId != "foa-acn-sn3d",
            "unlabelled four-channel WAV remains an ordinary speaker layout");

    testRoot.deleteRecursively();
}

double binauralEnergyForDirection(const AmbisonicBinauralFilterBank& bank,
                                  int ear,
                                  float yCoefficient,
                                  float xCoefficient)
{
    auto energy = 0.0;
    for (auto tap = 0; tap < bank.getTapCount(); ++tap)
    {
        const auto sample = bank.getSample(1, ear, 0, tap)
                          + (yCoefficient * bank.getSample(1, ear, 1, tap))
                          + (xCoefficient * bank.getSample(1, ear, 3, tap));
        energy += static_cast<double>(sample) * sample;
    }
    return energy;
}

void runOutputMeterAnalyzerTest()
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto blockSize = 4800;
    const auto layout = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(8);
    std::array<SpeakerRole, OutputMeterSnapshot::maxChannels> roles {};
    for (int channel = 0; channel < juce::jmin(8, static_cast<int>(layout.selectedRoles.size())); ++channel)
        roles[static_cast<std::size_t>(channel)] = layout.selectedRoles[static_cast<std::size_t>(channel)];

    OutputMeterAnalyzer analyzer;
    analyzer.prepare(sampleRate, 8, roles);

    juce::AudioBuffer<float> block(8, blockSize);
    block.clear();
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto value = static_cast<float>(0.1 * std::sin(juce::MathConstants<double>::twoPi
                                                             * 1000.0 * static_cast<double>(sample) / sampleRate));
        block.setSample(0, sample, value); // L
        block.setSample(3, sample, value); // LFE
        block.setSample(4, sample, value); // Lss
    }

    OutputMeterSnapshot snapshot;
    for (int blockIndex = 0; blockIndex < 4; ++blockIndex)
        snapshot = analyzer.process(block, 0, blockSize);

    require(snapshot.channelCount == 8, "output meter exposes the actual eight output channels");
    require(snapshot.truePeakLinear[0] > 0.09f && snapshot.rmsLinear[0] > 0.06f,
            "output meter measures per-channel peak and RMS");
    require(snapshot.truePeakLinear[3] > 0.09f
                && snapshot.kWeightedContributionDb[3] <= -119.0f,
            "LFE remains visible in peak metering but is excluded from standard loudness");
    require(snapshot.kWeightedContributionDb[4]
                > snapshot.kWeightedContributionDb[0] + 1.35f,
            "side-surround K-weighted contribution carries the BS.1770 +1.5 dB weighting");
    require(snapshot.momentaryLufs > snapshot.kWeightedContributionDb[4],
            "global momentary LUFS is the energy sum of active weighted contributions");
}

void runAmbisonicFieldAnalyzerTest()
{
    auto foaLayout = ChannelLayoutCatalog::layoutForPresetId(4, "foa-acn-sn3d");
    juce::AudioBuffer<float> frontField(4, 480);
    frontField.clear();
    for (int sample = 0; sample < frontField.getNumSamples(); ++sample)
    {
        frontField.setSample(0, sample, 0.25f); // ACN0 W
        frontField.setSample(3, sample, 0.25f); // ACN3 X, front
    }

    AmbisonicFieldAnalyzer analyzer;
    analyzer.prepare(48000.0);
    analyzer.process(frontField,
                     0,
                     frontField.getNumSamples(),
                     foaLayout,
                     {},
                     ChannelPlaybackState {});
    const auto frontSnapshot = analyzer.getSnapshot();
    require(frontSnapshot.valid && frontSnapshot.order == 1 && frontSnapshot.channelCount == 4,
            "FOA field analyzer publishes an ACN/SN3D snapshot");
    require(AmbisonicFieldAnalyzer::energyAt(frontSnapshot, 0.0f, 0.0f)
                > AmbisonicFieldAnalyzer::energyAt(frontSnapshot, 180.0f, 0.0f) * 20.0f,
            "FOA field projection preserves a strong front-facing energy lobe");

    analyzer.process(frontField,
                     0,
                     frontField.getNumSamples(),
                     foaLayout,
                     { 90.0f, 0.0f, 0.0f },
                     ChannelPlaybackState {});
    const auto yawedSnapshot = analyzer.getSnapshot();
    require(AmbisonicFieldAnalyzer::energyAt(yawedSnapshot, 90.0f, 0.0f)
                > AmbisonicFieldAnalyzer::energyAt(yawedSnapshot, -90.0f, 0.0f) * 20.0f,
            "positive yaw rotates the monitored field toward the left hemisphere");

    ChannelPlaybackState soloW;
    soloW.soloedBySourceChannel[0] = true;
    analyzer.requestReset();
    analyzer.process(frontField,
                     0,
                     frontField.getNumSamples(),
                     foaLayout,
                     {},
                     soloW);
    const auto soloWSnapshot = analyzer.getSnapshot();
    require(std::abs(AmbisonicFieldAnalyzer::energyAt(soloWSnapshot, 0.0f, 0.0f)
                     - AmbisonicFieldAnalyzer::energyAt(soloWSnapshot, 180.0f, 0.0f))
                < 0.0001f,
            "solo ACN0/W publishes an honest omnidirectional field instead of the full source");

    const auto quadLayout = ChannelLayoutCatalog::layoutForPresetId(4, "quad-lrlsrs");
    analyzer.process(frontField,
                     0,
                     frontField.getNumSamples(),
                     quadLayout,
                     {},
                     ChannelPlaybackState {});
    require(!analyzer.getSnapshot().valid,
            "ordinary Quad never publishes an Ambisonic field snapshot");
}

void runAmbisonicBinauralFilterBankTest()
{
    const auto resource = juce::File::getCurrentWorkingDirectory()
                              .getChildFile("SingleEditorJuce")
                              .getChildFile("Resources")
                              .getChildFile("Ambisonics")
                              .getChildFile("SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k.sehrir");
    juce::String error;
    const auto bank = AmbisonicBinauralFilterBank::loadFromFile(resource, error);
    require(bank != nullptr && error.isEmpty(), "loads the checked SADIE binaural filter resource");
    require(bank->getSampleRate() == 48000 && bank->getTapCount() == 256,
            "binaural resource exposes its fixed sample rate and tap count");
    require(bank->getSourceChannelCount(1) == 4
                && bank->getSourceChannelCount(2) == 9
                && bank->getSourceChannelCount(3) == 16,
            "binaural resource contains first through third-order ACN banks");
    require(bank->getSourceMd5() == "05aaa22e2d5654cbdee5cbe83ebfb1ef",
            "binaural resource retains the pinned SADIE source identity");
    const auto packed = AmbisonicBinauralFilterBank::loadDefault(error);
    require(packed && packed->getResourceIdentity() == bank->getResourceIdentity()
                && packed->getSampleRate() == bank->getSampleRate() && packed->getTapCount() == bank->getTapCount(),
            "typed owner pack preserves the raw bank's complete DSP identity");
    for (int order = 1; order <= 3; ++order)
        for (int ear = 0; ear < 2; ++ear)
            for (int channel = 0; channel < bank->getSourceChannelCount(order); ++channel)
                for (int tap = 0; tap < bank->getTapCount(); ++tap)
                    require(packed->getSample(order, ear, channel, tap) == bank->getSample(order, ear, channel, tap),
                            "all first/second/third-order packed HRIR samples remain exactly unchanged");

    const auto frontLeft = binauralEnergyForDirection(*bank, 0, 0.0f, 1.0f);
    const auto frontRight = binauralEnergyForDirection(*bank, 1, 0.0f, 1.0f);
    const auto leftAtLeftEar = binauralEnergyForDirection(*bank, 0, 1.0f, 0.0f);
    const auto leftAtRightEar = binauralEnergyForDirection(*bank, 1, 1.0f, 0.0f);
    const auto rightAtLeftEar = binauralEnergyForDirection(*bank, 0, -1.0f, 0.0f);
    const auto rightAtRightEar = binauralEnergyForDirection(*bank, 1, -1.0f, 0.0f);
    require(frontLeft > 0.001 && frontRight > 0.001
                && frontLeft / frontRight > 0.5 && frontLeft / frontRight < 2.0,
            "front HRIR remains audible and broadly symmetric");
    require(leftAtLeftEar > leftAtRightEar * 2.0,
            "left HRIR contains a real ipsilateral energy cue");
    require(rightAtRightEar > rightAtLeftEar * 2.0,
            "right HRIR contains a real ipsilateral energy cue");

    const auto corrupt = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("single-editor-corrupt-hrir", ".sehrir");
    require(resource.copyFileTo(corrupt), "copies binaural resource for corruption test");
    {
        std::unique_ptr<juce::FileOutputStream> output(corrupt.createOutputStream(128));
        require(output != nullptr && output->writeByte(0x7f), "corrupts binaural resource payload");
    }
    error.clear();
    require(AmbisonicBinauralFilterBank::loadFromFile(corrupt, error) == nullptr
                && error.isNotEmpty(),
            "binaural loader rejects a payload with a mismatched checksum");
    corrupt.deleteFile();
}

void runAmbisonicBinauralRendererTest()
{
    const auto resource = juce::File::getCurrentWorkingDirectory()
                              .getChildFile("SingleEditorJuce/Resources/Ambisonics")
                              .getChildFile("SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k.sehrir");
    juce::String error;
    const auto bank = AmbisonicBinauralFilterBank::loadFromFile(resource, error);
    require(bank != nullptr, "loads binaural bank for renderer test");

    juce::AudioBuffer<float> source(4, 512);
    source.clear();
    source.setSample(0, 0, 1.0f);
    source.setSample(3, 0, 1.0f);

    auto renderer = AmbisonicBinauralRenderer::create(bank, 1, {}, 48000.0, 512, error);
    require(renderer != nullptr && renderer->getLatencySamples() <= 64,
            "creates a low-latency first-order binaural renderer");
    juce::AudioBuffer<float> whole(2, 512);
    whole.clear();
    renderer->process(source, 0, whole, 0, 512, ChannelPlaybackState {});
    require(whole.getMagnitude(0, 0, 512) > 0.01f
                && whole.getMagnitude(1, 0, 512) > 0.01f,
            "binaural renderer emits both ears from a front FOA impulse");

    auto chunkedRenderer = AmbisonicBinauralRenderer::create(bank, 1, {}, 48000.0, 512, error);
    juce::AudioBuffer<float> chunked(2, 512);
    chunked.clear();
    chunkedRenderer->process(source, 0, chunked, 0, 197, ChannelPlaybackState {});
    chunkedRenderer->process(source, 197, chunked, 197, 315, ChannelPlaybackState {});
    for (auto channel = 0; channel < 2; ++channel)
    {
        for (auto sample = 0; sample < 512; ++sample)
        {
            require(std::abs(whole.getSample(channel, sample) - chunked.getSample(channel, sample))
                        < 1.0e-5f,
                    "binaural streaming output is independent of callback chunking");
            require(std::isfinite(whole.getSample(channel, sample)),
                    "binaural renderer output remains finite");
        }
    }

    auto yawedRenderer = AmbisonicBinauralRenderer::create(
        bank,
        1,
        AmbisonicOrientation { 90.0f, 0.0f, 0.0f },
        48000.0,
        512,
        error);
    juce::AudioBuffer<float> yawed(2, 512);
    yawed.clear();
    yawedRenderer->process(source, 0, yawed, 0, 512, ChannelPlaybackState {});
    const auto leftEnergy = yawed.getRMSLevel(0, 0, 512);
    const auto rightEnergy = yawed.getRMSLevel(1, 0, 512);
    require(leftEnergy > rightEnergy * 1.4f,
            "positive yaw rotates a front FOA impulse toward the left ear");

    juce::AudioBuffer<float> sustainedSource(4, 512);
    sustainedSource.clear();
    for (auto sample = 0; sample < sustainedSource.getNumSamples(); ++sample)
    {
        sustainedSource.setSample(0, sample, 0.25f);
        sustainedSource.setSample(3, sample, 0.25f);
    }

    auto liveRenderer = AmbisonicBinauralRenderer::create(bank, 1, {}, 48000.0, 512, error);
    auto identityReference = AmbisonicBinauralRenderer::create(bank, 1, {}, 48000.0, 512, error);
    auto yawedReference = AmbisonicBinauralRenderer::create(
        bank, 1, AmbisonicOrientation { 90.0f, 0.0f, 0.0f }, 48000.0, 512, error);
    require(liveRenderer != nullptr && identityReference != nullptr && yawedReference != nullptr,
            "creates live and reference binaural renderers");

    juce::AudioBuffer<float> warmup(2, 512);
    juce::AudioBuffer<float> identityWarmup(2, 512);
    juce::AudioBuffer<float> yawedWarmup(2, 512);
    liveRenderer->process(sustainedSource, 0, warmup, 0, 512, ChannelPlaybackState {});
    identityReference->process(sustainedSource, 0, identityWarmup, 0, 512, ChannelPlaybackState {});
    yawedReference->process(sustainedSource, 0, yawedWarmup, 0, 512, ChannelPlaybackState {});

    liveRenderer->setOrientation({ 90.0f, 0.0f, 0.0f });
    juce::AudioBuffer<float> transition(2, 512);
    juce::AudioBuffer<float> identityContinuation(2, 512);
    juce::AudioBuffer<float> yawedContinuation(2, 512);
    liveRenderer->process(sustainedSource, 0, transition, 0, 512, ChannelPlaybackState {});
    identityReference->process(sustainedSource, 0, identityContinuation, 0, 512, ChannelPlaybackState {});
    yawedReference->process(sustainedSource, 0, yawedContinuation, 0, 512, ChannelPlaybackState {});
    require(std::abs(transition.getSample(0, 0) - identityContinuation.getSample(0, 0)) < 1.0e-5f,
            "live binaural orientation starts the update block from the previous matrix");
    require(std::abs(transition.getRMSLevel(0, 0, 512)
                     - identityContinuation.getRMSLevel(0, 0, 512)) > 0.0001f,
            "live binaural orientation changes output without replacing the renderer");

    juce::AudioBuffer<float> settled(2, 512);
    juce::AudioBuffer<float> yawedSettled(2, 512);
    liveRenderer->process(sustainedSource, 0, settled, 0, 512, ChannelPlaybackState {});
    yawedReference->process(sustainedSource, 0, yawedSettled, 0, 512, ChannelPlaybackState {});
    for (auto ear = 0; ear < 2; ++ear)
    {
        for (auto sample = 384; sample < 512; ++sample)
        {
            require(std::abs(settled.getSample(ear, sample)
                             - yawedSettled.getSample(ear, sample)) < 1.0e-5f,
                    "live binaural rotation converges to the target orientation without resetting convolution");
        }
    }

    ChannelPlaybackState muted;
    muted.mutedBySourceChannel.fill(true);
    auto mutedRenderer = AmbisonicBinauralRenderer::create(bank, 1, {}, 48000.0, 512, error);
    juce::AudioBuffer<float> silence(2, 512);
    silence.clear();
    mutedRenderer->process(source, 0, silence, 0, 512, muted);
    require(silence.getMagnitude(0, 0, 512) == 0.0f
                && silence.getMagnitude(1, 0, 512) == 0.0f,
            "binaural renderer honours source-channel mute");
}

bool hasGeometryDiagnostic(const SpeakerGeometryValidationResult& result,
                           const juce::String& code)
{
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&code] (const auto& diagnostic)
    {
        return diagnostic.code == code;
    });
}

SpeakerGeometryProfile makeCustomFiveOneGeometry(int64_t revision = 7)
{
    const auto standard = AmbisonicStandardMatrixCatalog::standardProfile("standard-5.1-v1");
    require(standard != nullptr, "loads read-only standard 5.1 geometry template");
    auto profile = *standard;
    profile.profileId = "studio-a-custom-v1";
    profile.revision = revision;
    profile.displayName = "Studio A Custom";
    profile.parentTemplateId = standard->profileId;
    profile.builtInReadOnly = false;
    return profile;
}

void runAmbisonicSpeakerGeometryTest()
{
    AmbisonicStandardMatrixCatalog::resetDiagnostics();
    AmbisonicSpeakerGeometryGenerator::resetDiagnostics();
    const auto ordinaryStereo = ChannelLayoutCatalog::stereoOutputLayout();
    const auto ordinaryMatrix = ChannelRouter::buildRoutingMatrix(
        ordinaryStereo, ordinaryStereo, ChannelPlaybackState {});
    require(ordinaryMatrix.outputChannels == 2
                && AmbisonicStageGraph::create(ordinaryStereo, {}) == nullptr,
            "ordinary stereo remains outside the spatial stage graph");
    require(AmbisonicStandardMatrixCatalog::getLookupCountForDiagnostics() == 0
                && AmbisonicSpeakerGeometryGenerator::getGenerationCountForDiagnostics() == 0,
            "ordinary media does not touch matrix artifacts or geometry generation");

    juce::String artifactFailure;
    if (!AmbisonicStandardMatrixCatalog::verifyAllArtifacts(artifactFailure))
    {
        std::cerr << "Artifact verification detail: " << artifactFailure << "\n";
        require(false, "all checked-in standard matrix artifacts match their provenance hashes");
    }
    const auto profileIds = AmbisonicStandardMatrixCatalog::standardProfileIds();
    require(profileIds.size() == 6, "standard catalog exposes six versioned geometry profiles");

    const std::array<ChannelLayoutState, 6> layouts {
        ChannelLayoutCatalog::monoOutputLayout(),
        ChannelLayoutCatalog::stereoOutputLayout(),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(4),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(6),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(8),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12)
    };
    std::set<std::string> artifactIds;
    std::set<std::string> cacheKeys;
    for (const auto& layout : layouts)
    {
        for (auto order = 1; order <= 3; ++order)
        {
            const auto view = AmbisonicStandardMatrixCatalog::find(layout, order);
            require(view.has_value(), "standard catalog resolves every layout/order pair");
            require(view->outputChannels == static_cast<int>(layout.selectedRoles.size())
                        && view->sourceChannels == (order + 1) * (order + 1),
                    "standard artifact dimensions match canonical bus and Ambisonic order");
            require(juce::String(view->profileHash).length() == 64
                        && juce::String(view->matrixHash).length() == 64
                        && juce::String(view->cacheKey).length() == 64,
                    "standard artifact carries complete reproducible identity");
            artifactIds.insert(view->artifactId);
            cacheKeys.insert(view->cacheKey);
            for (auto output = 0; output < view->outputChannels; ++output)
            {
                const auto isLfe = view->speakers[static_cast<std::size_t>(output)].role
                                 == SpeakerRole::LFE;
                for (auto source = 0; source < view->sourceChannels; ++source)
                {
                    const auto gain = view->gains[(output * view->sourceChannels) + source];
                    require(std::isfinite(gain) && std::abs(gain) <= 1.0f,
                            "standard decode gains are finite and statically bounded");
                    require(!isLfe || gain == 0.0f,
                            "standard directional decode never injects energy into LFE");
                }
            }
        }
    }
    require(artifactIds.size() == 18 && cacheKeys.size() == 18,
            "all standard layout/order artifacts and cache identities are independent");

    const auto standard = AmbisonicStandardMatrixCatalog::standardProfile("standard-7.1.4-v1");
    require(standard != nullptr && standard->builtInReadOnly
                && standard->speakers.size() == 12,
            "built-in geometry templates are immutable source material");

    auto codecProfile = makeCustomFiveOneGeometry();
    auto encoded = SpeakerGeometryProfileCodec::toVar(codecProfile);
    encoded.getDynamicObject()->setProperty("futureVendorField", "preserve-me");
    const auto parsed = SpeakerGeometryProfileCodec::fromVar(encoded);
    require(parsed.success && !parsed.profile.builtInReadOnly,
            "imported geometry is parsed as editable data, not trusted as built-in");
    const auto roundTrip = SpeakerGeometryProfileCodec::toVar(parsed.profile);
    require(roundTrip.getProperty("futureVendorField", {}).toString() == "preserve-me",
            "unknown future profile fields survive import and export");

    auto devicePolluted = encoded.clone();
    devicePolluted.getDynamicObject()->setProperty("deviceBinding", "ASIO:output-1");
    const auto pollutedParse = SpeakerGeometryProfileCodec::fromVar(devicePolluted);
    require(!pollutedParse.success
                && pollutedParse.errorCode == "device_binding_field_forbidden",
            "geometry profile rejects backend and physical-device ownership");

    auto invalid = makeCustomFiveOneGeometry(8);
    invalid.speakers[1].azimuthDegrees = invalid.speakers[0].azimuthDegrees;
    invalid.speakers[1].elevationDegrees = invalid.speakers[0].elevationDegrees;
    invalid.speakers[2].distanceMetres = std::numeric_limits<float>::quiet_NaN();
    const auto invalidResult = AmbisonicSpeakerGeometryGenerator::validate(invalid);
    require(!invalidResult.isValid()
                && hasGeometryDiagnostic(invalidResult, "speaker_direction_duplicate")
                && hasGeometryDiagnostic(invalidResult, "speaker_coordinate_non_finite"),
            "invalid and degenerate custom geometry returns actionable hard errors");

    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> activatedByCallback;
    auto acceptActivation = true;
    AmbisonicSpeakerGeometrySession session(
        [&activatedByCallback, &acceptActivation] (auto artifact)
        {
            activatedByCallback = std::move(artifact);
            return acceptActivation;
        });
    auto profile = makeCustomFiveOneGeometry();
    session.setDraft(profile);
    require(session.validateDraft().isValid()
                && session.getState() == SpeakerGeometrySessionState::validated,
            "custom geometry enters the explicit validated stage");
    const auto firstRequest = session.beginGeneration(3);
    require(firstRequest.has_value()
                && session.getState() == SpeakerGeometrySessionState::generating,
            "validated draft snapshots an immutable generation request");
    auto firstResult = AmbisonicSpeakerGeometryGenerator::generate(*firstRequest);
    const auto firstArtifact = firstResult.artifact;
    require(firstResult.succeeded()
                && session.acceptGenerationResult(std::move(firstResult))
                       == SpeakerGeometryGenerationStatus::ready
                && session.getCandidate() == firstArtifact,
            "successful generation installs a candidate without activating it");
    require(firstArtifact->decodeMatrix.outputChannels == 6
                && firstArtifact->decodeMatrix.sourceChannels == 16
                && firstArtifact->decodeMatrix.gain(3, 0) == 0.0f,
            "custom 3OA 5.1 artifact uses canonical dimensions and a zero LFE row");
    require(session.beginAudition() == firstArtifact && session.getActive() == nullptr,
            "audition previews the candidate without changing active decode");
    session.endAudition();
    require(session.getState() == SpeakerGeometrySessionState::candidate
                && session.getActive() == nullptr,
            "ending audition returns to candidate without committing it");
    require(session.activateCandidate() && session.getActive() == firstArtifact
                && activatedByCallback == firstArtifact,
            "activation atomically publishes only the validated candidate");

    auto rejectedProfile = profile;
    rejectedProfile.revision += 1;
    rejectedProfile.speakers[0].azimuthDegrees += 2.0f;
    session.setDraft(rejectedProfile);
    require(session.validateDraft().isValid(), "validates a second activation candidate");
    const auto rejectedRequest = session.beginGeneration(3);
    require(rejectedRequest.has_value(), "starts generation for the second activation candidate");
    auto rejectedResult = AmbisonicSpeakerGeometryGenerator::generate(*rejectedRequest);
    require(rejectedResult.succeeded()
                && session.acceptGenerationResult(std::move(rejectedResult))
                       == SpeakerGeometryGenerationStatus::ready,
            "installs the second candidate without replacing active");
    acceptActivation = false;
    require(!session.activateCandidate()
                && session.getState() == SpeakerGeometrySessionState::candidate
                && session.getActive() == firstArtifact,
            "rejected activation keeps the previous valid matrix active and candidate retryable");
    acceptActivation = true;

    session.setDraft(profile);
    const auto cachedRequest = session.beginGeneration(3);
    auto cachedResult = AmbisonicSpeakerGeometryGenerator::generate(*cachedRequest);
    require(session.acceptGenerationResult(std::move(cachedResult))
                == SpeakerGeometryGenerationStatus::ready
                && session.getCandidate() == firstArtifact
                && session.getCacheSizeForDiagnostics() == 2,
            "same generator identity reuses the immutable candidate cache without a third entry");

    const auto staleRequest = session.beginGeneration(2);
    profile.revision += 1;
    session.setDraft(profile);
    auto staleResult = AmbisonicSpeakerGeometryGenerator::generate(*staleRequest);
    require(session.acceptGenerationResult(std::move(staleResult))
                == SpeakerGeometryGenerationStatus::staleRevision
                && session.getActive() == firstArtifact,
            "stale worker result cannot replace the last-known-good active matrix");

    const auto unsupportedRequest = session.beginGeneration(4);
    auto unsupportedResult = AmbisonicSpeakerGeometryGenerator::generate(*unsupportedRequest);
    require(session.acceptGenerationResult(std::move(unsupportedResult))
                == SpeakerGeometryGenerationStatus::unsupportedOrder
                && session.getState() == SpeakerGeometrySessionState::error
                && session.getActive() == firstArtifact,
            "generation failure is explicit and preserves the previous valid decode");

    invalid.revision = profile.revision + 1;
    session.setDraft(invalid);
    require(!session.validateDraft().isValid() && session.getActive() == firstArtifact,
            "invalid edits never replace the active matrix");
}

void runDocumentSaveOverwriteTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorLogicTests")
                              .getNonexistentChildFile("save-overwrite", {});
    testRoot.createDirectory();

    const auto sourceFile = testRoot.getChildFile("source.wav");
    require(writeTestWavFile(sourceFile), "writes initial WAV test file");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads source WAV for destructive save test");
    require(document.applySelectionGain({ 0.0, 0.01 }, 0.0f), "applies destructive edit before save");
    require(document.hasUnsavedChanges(), "document reports unsaved changes before save");
    require(document.saveOverSourceFile(), "saves over the currently opened source file");
    require(!document.hasUnsavedChanges(), "document is clean after destructive save");
    require(SingleTrackDocument::isEnvelopeFlatUnity(document.getEnvelopePoints()), "destructive save bakes envelope into clean baseline");

    auto reader = SingleTrackDocument::createReaderForFile(sourceFile);
    require(reader != nullptr, "reopens overwritten WAV file");

    juce::AudioBuffer<float> verifyBuffer(1, 16);
    verifyBuffer.clear();
    require(reader->read(&verifyBuffer, 0, verifyBuffer.getNumSamples(), 0, true, false), "reads overwritten source samples");
    require(std::abs(verifyBuffer.getSample(0, 0)) < 0.001f, "overwritten file contains edited samples");

    sourceFile.deleteFile();
    testRoot.deleteRecursively();
}

void runDocumentBinauralModeTest()
{
    const auto initialReads = ambisonicResources::packReadAttempts.load();
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorLogicTests")
                              .getNonexistentChildFile("binaural-mode", {});
    testRoot.createDirectory();
    const auto sourceFile = testRoot.getChildFile("four-channel.wav");
    require(writeTestWavFile(sourceFile, 4), "writes four-channel Ambisonic mode fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads four-channel mode fixture");
    require(!document.getState()->info.channelLayout.isAmbisonic
                && document.getState()->ambisonicBinauralFilterBank == nullptr,
            "ordinary four-channel open does not load the HRTF resource");
    require(document.setChannelLayoutPreset("foa-acn-sn3d"),
            "explicitly interprets the source as FOA ACN/SN3D");
    require(document.getState()->ambisonicBinauralFilterBank == nullptr,
            "speaker FOA interpretation still leaves HRTF unloaded");
    require(ambisonicResources::packReadAttempts == initialReads,
            "ordinary open and speaker FOA do not even read an owner pack header");
    require(document.setAmbisonicMonitorMode(AmbisonicMonitorMode::binauralHrtf)
                && document.getState()->ambisonicBinauralFilterBank != nullptr
                && document.getAmbisonicBinauralStatus().isEmpty(),
            "explicit binaural selection loads the validated HRTF resource on demand");
    require(ambisonicResources::packReadAttempts == initialReads + 1, "one explicit HRTF selection reads one pack");
    {
        const auto before = document.getState();
        const auto prepared = before->ambisonicBinauralFilterBank;
        require(document.setAmbisonicOrientation({45.0f, 0.0f, 0.0f}), "change monitor orientation before late publication");
        const auto after = document.getState();
        require(before != after && before->changeGeneration == after->changeGeneration,
                "monitor state changed without a chronological generation increment");
        require(!document.applyPreparedBinauralMonitor(before, prepared, {}) && document.getState() == after,
                "a prepared HRTF result cannot overwrite a different exact document state");
    }
    const std::weak_ptr<const AmbisonicBinauralFilterBank> lifetime = document.getState()->ambisonicBinauralFilterBank;
    require(document.setAmbisonicMonitorMode(AmbisonicMonitorMode::speakerProjection)
                && document.getState()->ambisonicBinauralFilterBank == nullptr,
            "returning to speaker projection releases the HRTF resource");
    require(lifetime.expired(), "document-only bank lease is actually released after speaker projection");

    sourceFile.deleteFile();
    testRoot.deleteRecursively();
}

void runWorkspaceModuleLayoutTest()
{
    WorkspaceModuleLayout layout;
    layout.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    layout.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    require(layout.activeModules().empty(),
            "registered workspace modules remain inactive until explicitly available");

    layout.setAvailable(WorkspaceModuleId::outputMeter, true);
    auto active = layout.activeModules();
    require(active.size() == 1 && active.front() == WorkspaceModuleId::outputMeter,
            "ordinary full chrome activates only the available Output module");

    layout.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    active = layout.activeModules();
    require(active.size() == 2
                && active[0] == WorkspaceModuleId::ambisonicEnergy
                && active[1] == WorkspaceModuleId::outputMeter,
            "Ambisonic Energy joins the dock in persisted order only when available");

    const auto extents = layout.calculateActiveExtents(600, 10);
    require(extents.size() == 2 && extents[0] >= 180 && extents[1] >= 160
                && extents[0] + extents[1] == 590,
            "workspace weights fill the dock while preserving module minimum extents");

    layout.setEnabled(WorkspaceModuleId::ambisonicEnergy, false);
    const auto saved = layout.serialiseUserState();

    WorkspaceModuleLayout restored;
    restored.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    restored.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    restored.restoreUserState(saved);
    restored.setAvailable(WorkspaceModuleId::outputMeter, true);
    restored.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    active = restored.activeModules();
    require(active.size() == 1 && active.front() == WorkspaceModuleId::outputMeter,
            "disabled module preference survives availability changes and restore");

    restored.setEnabled(WorkspaceModuleId::ambisonicEnergy, true);
    restored.moveModuleToActiveIndex(WorkspaceModuleId::outputMeter, 0);
    restored.setWeight(WorkspaceModuleId::outputMeter, 420);
    restored.setWeight(WorkspaceModuleId::ambisonicEnergy, 920);
    active = restored.activeModules();
    require(active.size() == 2 && active[0] == WorkspaceModuleId::outputMeter,
            "title drag order is represented independently from module identity");
    const auto resizedExtents = restored.calculateActiveExtents(600, 10);
    require(resizedExtents[1] > resizedExtents[0]
                && resizedExtents[0] + resizedExtents[1] == 590,
            "resized module weights survive generic active-order layout");

    restored.setActiveExtents({ 250, 340 });
    const auto splitterExtents = restored.calculateActiveExtents(600, 10);
    require(std::abs(splitterExtents[0] - 250) <= 1
                && std::abs(splitterExtents[1] - 340) <= 1,
            "splitter extents round-trip through persistent workspace weights");

    const auto stableState = restored.serialiseUserState();
    restored.restoreUserState("v1|unknown:1:0:9999|output-meter:not-a-number");
    require(restored.serialiseUserState() == stableState,
            "unknown or malformed persisted module state is ignored atomically");

    WorkspaceModuleLayout defaults;
    defaults.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    defaults.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    defaults.setAvailable(WorkspaceModuleId::outputMeter, true);
    defaults.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    restored.resetUserState();
    active = restored.activeModules();
    if (restored.serialiseUserState() != defaults.serialiseUserState())
        std::cerr << "Reset actual: " << restored.serialiseUserState() << "\nReset expected: " << defaults.serialiseUserState() << '\n';
    require(restored.serialiseUserState() == defaults.serialiseUserState()
                && active.size() == 2
                && active[0] == WorkspaceModuleId::ambisonicEnergy,
            "reset restores registered user defaults without clearing runtime availability");
    WorkspaceModuleLayout optionalLayout;
    WorkspaceModuleDescriptor optional { WorkspaceModuleId::parametricEq, 0, 1000, 160 };
    optional.defaultPlaced = false;
    optionalLayout.registerModule(optional);
    optionalLayout.setAvailable(WorkspaceModuleId::parametricEq, true);
    require(optionalLayout.setModulePlaced(WorkspaceModuleId::parametricEq, true) == WorkspaceGridMutationResult::committed,
            "explicitly place an optional module before resetting its registered default");
    optionalLayout.resetUserState();
    require(!optionalLayout.isPlaced(WorkspaceModuleId::parametricEq) && optionalLayout.activeModules().empty(),
            "reset does not silently place a module whose registered default is unplaced");
    require(optionalLayout.setModulePlaced(WorkspaceModuleId::parametricEq, true) == WorkspaceGridMutationResult::committed,
            "optional module remains available for explicit placement after reset");
}

} // namespace

int main()
{
    const auto quad = ChannelLayoutCatalog::defaultLayoutForChannelCount(4);
    require(quad.selectedPresetId == "quad-lrlsrs", "4ch remains Quad by default");
    require(!quad.isAmbisonic, "unlabelled 4ch is not falsely detected as Ambisonics");
    require(hasCandidate(quad, "foa-acn-sn3d"), "4ch offers explicit FOA ACN/SN3D interpretation");

    const auto foa = ChannelLayoutCatalog::layoutForPresetId(4, "foa-acn-sn3d");
    require(foa.isAmbisonic && foa.ambisonicOrder == 1, "FOA interpretation exposes first order metadata");
    require(foa.ambisonicNormalization == "SN3D", "FOA interpretation exposes SN3D normalization");
    require(foa.selectedRoles.size() == 4
                && foa.selectedRoles.front() == SpeakerRole::ambiACN0
                && foa.selectedRoles.back() == SpeakerRole::ambiACN3,
            "FOA interpretation uses ACN0 through ACN3");

    require(AmbisonicMonitorDecoder::create(quad) == nullptr,
            "ordinary Quad does not initialise an Ambisonic decoder");
    require(AmbisonicStageGraph::create(ChannelLayoutCatalog::monoOutputLayout(), {}) == nullptr
                && AmbisonicStageGraph::create(ChannelLayoutCatalog::stereoOutputLayout(), {}) == nullptr
                && AmbisonicStageGraph::create(quad, {}) == nullptr,
            "ordinary mono, stereo and multichannel layouts create zero Ambisonics stages");
    const auto foaDecoder = AmbisonicMonitorDecoder::create(foa);
    require(foaDecoder != nullptr, "explicit FOA creates the monitor decoder on demand");
    require(AmbisonicMonitorDecoder::statusFor(foa) == AmbisonicMonitorDecodeStatus::ready,
            "FOA ACN/SN3D is reported ready for monitor decode");
    const auto foaStageGraph = AmbisonicStageGraph::create(foa, {});
    require(foaStageGraph != nullptr
                && foaStageGraph->getEffectiveOrder() == 1
                && foaStageGraph->getSourceChannelCount() == 4,
            "the canonical stage graph binds the truthful FOA order and channel count");
    require(AmbisonicStageGraph::stageOrderId()
                == "validate-convert>rotate>analyze>decode-max-re>canonical-speaker-bus>role-mapping>physical-device>meter"
                && foaStageGraph->getDecoderProfileId()
                       == "max-re-137.9deg-fixed-reference-v1",
            "the stage order and locked max-rE profile have versioned identities");
    require(near(AmbisonicMonitorDecoder::maxReWeightForDegree(1, 0), 1.0f)
                && AmbisonicMonitorDecoder::maxReWeightForDegree(1, 1) > 0.57f
                && AmbisonicMonitorDecoder::maxReWeightForDegree(1, 1) < 0.58f
                && near(AmbisonicMonitorDecoder::fixedReferenceGainForOrder(1), 0.50f),
            "FOA uses the locked degree weights and target-independent reference gain");

    auto wrongOrdering = foa;
    std::swap(wrongOrdering.selectedRoles[1], wrongOrdering.selectedRoles[3]);
    require(AmbisonicMonitorDecoder::statusFor(wrongOrdering)
                == AmbisonicMonitorDecodeStatus::unsupportedChannelOrdering
                && AmbisonicStageGraph::create(wrongOrdering, {}) == nullptr,
            "validation rejects Ambisonics that is not ordered ACN0 through ACN3");
    auto implicitAmbisonics = foa;
    implicitAmbisonics.interpretationOrigin = ChannelInterpretationOrigin::implicitDefault;
    implicitAmbisonics.detectedFromMetadata = false;
    require(AmbisonicMonitorDecoder::statusFor(implicitAmbisonics)
                == AmbisonicMonitorDecodeStatus::interpretationRequired,
            "validation rejects an Ambisonic guess without metadata or user interpretation");
    auto wrongNormalization = foa;
    wrongNormalization.ambisonicNormalization = "N3D";
    require(AmbisonicMonitorDecoder::statusFor(wrongNormalization)
                == AmbisonicMonitorDecodeStatus::unsupportedNormalization,
            "validation rejects non-SN3D input before decode");
    auto wrongChannelCount = foa;
    wrongChannelCount.sourceChannelCount = 3;
    require(AmbisonicMonitorDecoder::statusFor(wrongChannelCount)
                == AmbisonicMonitorDecodeStatus::unsupportedOrder,
            "validation rejects a channel count that cannot carry the declared order");

    const auto foaStereo = foaDecoder->buildRoutingMatrix(ChannelLayoutCatalog::stereoOutputLayout(),
                                                           ChannelPlaybackState {});
    require(foaStereo.sourceChannels == 4 && foaStereo.outputChannels == 2,
            "FOA decoder emits a four-to-two routing matrix");
    require(near(gainAt(foaStereo, 0, 0), gainAt(foaStereo, 1, 0)),
            "FOA W reaches stereo speakers symmetrically");
    require(near(gainAt(foaStereo, 0, 3), gainAt(foaStereo, 1, 3)),
            "front-facing FOA X reaches stereo speakers symmetrically");
    require(gainAt(foaStereo, 0, 1) > gainAt(foaStereo, 1, 1),
            "positive FOA Y favours the left speaker");
    require(maximumAbsoluteRowSum(foaStereo) <= 1.0001f,
            "FOA speaker projection reserves deterministic peak headroom");

    const auto canonicalSevenOneFour = foaStageGraph->buildCanonicalSpeakerBusMatrix(
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12),
        ChannelPlaybackState {});
    require(canonicalSevenOneFour.sourceChannels == 4
                && canonicalSevenOneFour.outputChannels == 12,
            "decode first publishes a semantic 7.1.4 canonical speaker bus");
    for (auto output = 0; output < canonicalSevenOneFour.outputChannels; ++output)
    {
        if (output == 3)
            require(near(gainAt(canonicalSevenOneFour, output, 0), 0.0f),
                    "canonical speaker bus keeps LFE directional coefficients at zero");
        else
            require(std::abs(gainAt(canonicalSevenOneFour, output, 0)) > 0.0001f,
                    "truthful low-order decode may energise every non-LFE canonical role");
    }
    require(near(gainAt(foaStereo, 0, 0), gainAt(canonicalSevenOneFour, 0, 0)),
            "the locked reference gain does not change with target speaker count");

    ChannelPlaybackState swappedPhysicalOutputs;
    const auto sevenOneFourLayout = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12);
    require(swappedPhysicalOutputs.outputMapping.setOutputChannel(SpeakerRole::L, 4)
                && swappedPhysicalOutputs.outputMapping.setOutputChannel(
                       sevenOneFourLayout.selectedRoles[4], 0),
            "builds an explicit canonical-role to physical-output swap");
    const auto physicalSevenOneFour = foaStageGraph->buildPhysicalRoutingMatrix(
        sevenOneFourLayout,
        swappedPhysicalOutputs);
    require(near(gainAt(physicalSevenOneFour, 4, 0),
                 gainAt(canonicalSevenOneFour, 0, 0))
                && near(gainAt(physicalSevenOneFour, 0, 0),
                        gainAt(canonicalSevenOneFour, 4, 0)),
            "role mapping is applied after canonical decode rather than inside it");

    const auto yawedFoaDecoder = AmbisonicMonitorDecoder::create(
        foa,
        AmbisonicOrientation { 90.0f, 0.0f, 0.0f });
    require(yawedFoaDecoder != nullptr, "FOA yaw creates a rotated decoder snapshot");
    const auto yawedFoaStereo = yawedFoaDecoder->buildRoutingMatrix(
        ChannelLayoutCatalog::stereoOutputLayout(),
        ChannelPlaybackState {});
    require(gainAt(yawedFoaStereo, 0, 3) > gainAt(yawedFoaStereo, 1, 3),
            "positive yaw rotates a front FOA X source toward the left speaker");
    require(maximumAbsoluteRowSum(yawedFoaStereo) <= 1.0001f,
            "yaw rotation preserves deterministic peak headroom");

    const auto foaSevenOneFour = foaDecoder->buildRoutingMatrix(
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12),
        ChannelPlaybackState {});
    require(foaSevenOneFour.outputChannels == 12, "FOA decoder follows a 7.1.4 output target");
    require(gainAt(foaSevenOneFour, 8, 2) > gainAt(foaSevenOneFour, 0, 2),
            "positive FOA Z favours the top-front speaker over the floor speaker");
    require(near(gainAt(foaSevenOneFour, 3, 0), 0.0f),
            "FOA monitor decode never feeds LFE");

    const auto pitchedFoaDecoder = AmbisonicMonitorDecoder::create(
        foa,
        AmbisonicOrientation { 0.0f, 45.0f, 0.0f });
    const auto pitchedFoa = pitchedFoaDecoder->buildRoutingMatrix(
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12),
        ChannelPlaybackState {});
    require(gainAt(pitchedFoa, 8, 3) > gainAt(pitchedFoa, 0, 3),
            "positive pitch rotates a front FOA X source toward top-front");

    const auto rolledFoaDecoder = AmbisonicMonitorDecoder::create(
        foa,
        AmbisonicOrientation { 0.0f, 0.0f, 45.0f });
    const auto rolledFoa = rolledFoaDecoder->buildRoutingMatrix(
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12),
        ChannelPlaybackState {});
    require(gainAt(rolledFoa, 8, 1) > gainAt(rolledFoa, 4, 1),
            "positive roll rotates a left FOA Y source toward top-left");

    constexpr auto changed = 0.01f;
    constexpr auto unchanged = 0.0001f;
    const AmbisonicOrientation yaw90 { 90.0f, 0.0f, 0.0f };
    const AmbisonicOrientation pitch90 { 0.0f, 90.0f, 0.0f };
    const AmbisonicOrientation roll90 { 0.0f, 0.0f, 90.0f };
    require(firstOrderBasisDifference(0, yaw90) < unchanged
                && firstOrderBasisDifference(0, pitch90) < unchanged
                && firstOrderBasisDifference(0, roll90) < unchanged,
            "ACN0/W remains omnidirectional for every orientation axis");
    require(firstOrderBasisDifference(1, yaw90) > changed
                && firstOrderBasisDifference(1, pitch90) < unchanged
                && firstOrderBasisDifference(1, roll90) > changed,
            "ACN1/Y responds to yaw and roll but remains invariant around the pitch axis");
    require(firstOrderBasisDifference(2, yaw90) < unchanged
                && firstOrderBasisDifference(2, pitch90) > changed
                && firstOrderBasisDifference(2, roll90) > changed,
            "ACN2/Z responds to pitch and roll but remains invariant around the yaw axis");
    require(firstOrderBasisDifference(3, yaw90) > changed
                && firstOrderBasisDifference(3, pitch90) > changed
                && firstOrderBasisDifference(3, roll90) < unchanged,
            "ACN3/X responds to yaw and pitch but remains invariant around the roll axis");

    ChannelPlaybackState muteFoaY;
    muteFoaY.mutedBySourceChannel[1] = true;
    const auto mutedFoa = foaDecoder->buildRoutingMatrix(ChannelLayoutCatalog::stereoOutputLayout(),
                                                          muteFoaY);
    require(near(gainAt(mutedFoa, 0, 1), 0.0f) && near(gainAt(mutedFoa, 1, 1), 0.0f),
            "FOA monitor decode honours source-channel mute");

    const auto twoOaDefault = ChannelLayoutCatalog::defaultLayoutForChannelCount(9);
    require(twoOaDefault.selectedPresetId == "discrete-9", "9ch remains Discrete by default");
    require(!twoOaDefault.isAmbisonic, "unlabelled 9ch is not falsely detected as 2OA");
    require(hasCandidate(twoOaDefault, "2oa-acn-sn3d"), "9ch offers explicit 2OA ACN/SN3D interpretation");
    const auto twoOa = ChannelLayoutCatalog::layoutForPresetId(9, "2oa-acn-sn3d");
    require(twoOa.isAmbisonic && twoOa.ambisonicOrder == 2, "2OA interpretation exposes second order metadata");
    require(twoOa.selectedRoles.size() == 9
                && twoOa.selectedRoles.back() == SpeakerRole::ambiACN8,
            "2OA interpretation uses ACN0 through ACN8");
    const auto twoOaDecoder = AmbisonicMonitorDecoder::create(twoOa);
    require(twoOaDecoder != nullptr
                && AmbisonicMonitorDecoder::statusFor(twoOa)
                       == AmbisonicMonitorDecodeStatus::ready,
            "2OA ACN/SN3D creates its monitor decoder on demand");
    const auto twoOaStageGraph = AmbisonicStageGraph::create(twoOa, {});
    require(twoOaStageGraph != nullptr
                && twoOaStageGraph->getEffectiveOrder() == 2
                && twoOaStageGraph->getSourceChannelCount() == 9
                && AmbisonicMonitorDecoder::maxReWeightForDegree(2, 1)
                       > AmbisonicMonitorDecoder::maxReWeightForDegree(2, 2),
            "2OA keeps its true order and locked max-rE degree taper");
    const auto twoOaStereo = twoOaDecoder->buildRoutingMatrix(
        ChannelLayoutCatalog::stereoOutputLayout(),
        ChannelPlaybackState {});
    require(twoOaStereo.sourceChannels == 9 && twoOaStereo.outputChannels == 2,
            "2OA decoder exposes all nine ACN components");
    require(maximumAbsoluteRowSum(twoOaStereo) <= 1.0001f,
            "2OA speaker projection cannot exceed unity for bounded components");

    const auto threeOaDefault = ChannelLayoutCatalog::defaultLayoutForChannelCount(16);
    require(threeOaDefault.selectedPresetId == "discrete-16", "16ch remains Discrete by default");
    require(!threeOaDefault.isAmbisonic, "unlabelled 16ch is not falsely detected as 3OA");
    require(hasCandidate(threeOaDefault, "3oa-acn-sn3d"), "16ch offers explicit 3OA ACN/SN3D interpretation");
    const auto threeOa = ChannelLayoutCatalog::layoutForPresetId(16, "3oa-acn-sn3d");
    require(threeOa.isAmbisonic && threeOa.ambisonicOrder == 3, "3OA interpretation exposes third order metadata");
    require(threeOa.selectedRoles.size() == 16
                && threeOa.selectedRoles.back() == SpeakerRole::ambiACN15,
            "3OA interpretation uses ACN0 through ACN15");
    const auto threeOaDecoder = AmbisonicMonitorDecoder::create(threeOa);
    require(threeOaDecoder != nullptr
                && AmbisonicMonitorDecoder::statusFor(threeOa)
                       == AmbisonicMonitorDecodeStatus::ready,
            "3OA ACN/SN3D creates its monitor decoder on demand");
    const auto threeOaStageGraph = AmbisonicStageGraph::create(threeOa, {});
    require(threeOaStageGraph != nullptr
                && threeOaStageGraph->getEffectiveOrder() == 3
                && threeOaStageGraph->getSourceChannelCount() == 16,
            "3OA keeps all sixteen ACN components without truncating the matrix");
    const auto threeOaSevenOneFour = threeOaDecoder->buildRoutingMatrix(
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12),
        ChannelPlaybackState {});
    require(threeOaSevenOneFour.sourceChannels == 16
                && threeOaSevenOneFour.outputChannels == 12,
            "3OA decoder exposes all sixteen ACN components to 7.1.4");
    require(maximumAbsoluteRowSum(threeOaSevenOneFour) <= 1.0001f,
            "3OA speaker projection cannot exceed unity for bounded components");
    for (int output = 0; output < threeOaSevenOneFour.outputChannels; ++output)
    {
        for (int source = 0; source < threeOaSevenOneFour.sourceChannels; ++source)
            require(std::isfinite(gainAt(threeOaSevenOneFour, output, source)),
                    "higher-order decoder matrix remains finite");
    }
    require(threeOa.orderText.startsWith("ACN0 ACN1")
                && threeOa.orderText.endsWith("ACN14 ACN15"),
            "3OA order text remains complete across compiler argument ordering");
    require(ChannelLayoutCatalog::roleFromAbbreviation("acn15") == SpeakerRole::ambiACN15,
            "ACN15 role parsing is case insensitive");

    const auto fiveOne = ChannelLayoutCatalog::defaultLayoutForChannelCount(6);
    require(fiveOne.selectedPresetId == "5.1-smpte-dolby", "6ch defaults to 5.1 SMPTE/Dolby");
    require(fiveOne.interpretationOrigin == ChannelInterpretationOrigin::implicitDefault,
            "ordinary default source interpretation records implicit provenance");
    require(fiveOne.selectedRoles.size() == 6, "5.1 has 6 roles");
    require(fiveOne.selectedRoles[0] == SpeakerRole::L, "5.1 ch1 is L");
    require(fiveOne.selectedRoles[1] == SpeakerRole::R, "5.1 ch2 is R");
    require(fiveOne.selectedRoles[2] == SpeakerRole::C, "5.1 ch3 is C");
    require(fiveOne.selectedRoles[3] == SpeakerRole::LFE, "5.1 ch4 is LFE");

    ChannelLayoutCatalog::setPreferredDefaultPresetId(6, "5.1-film-protools");
    const auto fiveOneFilmDefault = ChannelLayoutCatalog::defaultLayoutForChannelCount(6);
    require(fiveOneFilmDefault.selectedPresetId == "5.1-film-protools", "6ch preferred default can be Film");
    require(fiveOneFilmDefault.selectedRoles[1] == SpeakerRole::C, "5.1 Film ch2 is C");
    require(fiveOneFilmDefault.orderText == "L C R Ls Rs LFE",
            "5.1 Film/Pro Tools source order is deterministic");
    ChannelLayoutCatalog::setPreferredDefaultPresetId(6, {});

    const auto sevenOneFour = ChannelLayoutCatalog::defaultLayoutForChannelCount(12);
    require(sevenOneFour.selectedPresetId == "7.1.4-dolby-smpte", "12ch defaults to 7.1.4 Dolby/SMPTE");
    require(sevenOneFour.selectedRoles[8] == SpeakerRole::Ltf, "7.1.4 ch9 is Ltf");
    require(sevenOneFour.selectedRoles[11] == SpeakerRole::Rtr, "7.1.4 ch12 is Rtr");
    const auto sevenOneFourFilm = ChannelLayoutCatalog::layoutForPresetId(
        12,
        "7.1.4-film-protools");
    require(sevenOneFourFilm.orderText
                == "L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr",
            "7.1.4 Film/Pro Tools source order is deterministic");
    const auto wavSevenOneFour = ChannelLayoutCatalog::layoutForWaveChannelMask(12, 0x2d63f);
    require(wavSevenOneFour.interpretationOrigin
                == ChannelInterpretationOrigin::waveChannelMask
                && wavSevenOneFour.orderText
                       == "L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr",
            "WAV 7.1.4 mask maps every file channel to a deterministic role");

    const auto microsoftWaveSevenOne = ChannelLayoutCatalog::layoutForPresetId(8, "7.1-microsoft-wave");
    require(microsoftWaveSevenOne.selectedPresetId == "7.1-microsoft-wave", "8ch exposes Microsoft WAVE preset");
    require(microsoftWaveSevenOne.interpretationOrigin
                == ChannelInterpretationOrigin::explicitPreset,
            "explicit source preset records explicit provenance");
    require(microsoftWaveSevenOne.selectedRoles[4] == SpeakerRole::Lsr, "Microsoft WAVE ch5 is rear left");
    require(microsoftWaveSevenOne.selectedRoles[6] == SpeakerRole::Lss, "Microsoft WAVE ch7 is side left");

    const auto wavSevenOne = ChannelLayoutCatalog::layoutForWaveChannelMask(8, 1599);
    require(wavSevenOne.detectedFromMetadata, "WAV channel mask is metadata-detected");
    require(wavSevenOne.interpretationOrigin == ChannelInterpretationOrigin::waveChannelMask
                && wavSevenOne.waveChannelMask == 1599,
            "WAV mask interpretation records its versioned metadata provenance");
    require(wavSevenOne.selectedRoles[4] == SpeakerRole::Lsr, "WAV 7.1 mask ch5 is rear left");
    require(wavSevenOne.selectedRoles[6] == SpeakerRole::Lss, "WAV 7.1 mask ch7 is side left");
    require(! hasCandidate(wavSevenOne, "7.1-microsoft-wave"), "Detected 0x63F hides duplicate static WAVE preset");
    const auto staticWaveWithMatchingMask = ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(8,
                                                                                                        1599,
                                                                                                        "7.1-microsoft-wave");
    require(staticWaveWithMatchingMask.selectedPresetId == wavSevenOne.selectedPresetId,
            "Matching static WAVE selection normalises to detected metadata preset");
    const auto smpteWithMaskCandidate = ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(8,
                                                                                                   1599,
                                                                                                   "7.1-smpte");
    require(smpteWithMaskCandidate.selectedPresetId == "7.1-smpte", "Can switch WAV mask file to 7.1 SMPTE");
    require(smpteWithMaskCandidate.interpretationOrigin
                == ChannelInterpretationOrigin::explicitPreset
                && smpteWithMaskCandidate.waveChannelMask == 1599,
            "explicit override remains distinct from the detected WAV candidate");
    require(hasCandidate(smpteWithMaskCandidate, wavSevenOne.selectedPresetId), "WAV channel mask option remains available");

    ChannelLayoutCatalog::setPreferredDefaultPresetId(8, "7.1-film-protools");
    const auto preferredSevenOne = ChannelLayoutCatalog::defaultLayoutForChannelCount(8);
    require(preferredSevenOne.selectedPresetId == "7.1-film-protools", "8ch preferred default can be Film");
    require(preferredSevenOne.orderText == "L C R Lss Rss Lsr Rsr LFE",
            "7.1 Film/Pro Tools source order is deterministic");
    const auto deviceSevenOne = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(8);
    require(deviceSevenOne.selectedPresetId == "windows-wave-7.1", "device 7.1 order ignores source-order preference");
    require(deviceSevenOne.selectedRoles[4] == SpeakerRole::Lss, "device 7.1 ch5 is side left");
    require(deviceSevenOne.selectedRoles[6] == SpeakerRole::Lsr, "device 7.1 ch7 is rear left");
    ChannelLayoutCatalog::setPreferredDefaultPresetId(8, {});

    const auto discrete = ChannelLayoutCatalog::defaultLayoutForChannelCount(10);
    require(discrete.isDiscreteFallback, "10ch falls back to Discrete");
    require(discrete.selectedPresetId == "discrete-10", "10ch discrete id");

    const auto stereoTarget = ChannelLayoutCatalog::stereoOutputLayout();
    const auto fiveOneToStereo = ChannelRouter::buildRoutingMatrix(fiveOne,
                                                                   stereoTarget,
                                                                   ChannelPlaybackState {});
    require(fiveOneToStereo.sourceChannels == 6, "5.1 to stereo source count");
    require(fiveOneToStereo.outputChannels == 2, "5.1 to stereo output count");
    require(near(gainAt(fiveOneToStereo, 0, 0), 1.0f), "L maps to stereo L");
    require(near(gainAt(fiveOneToStereo, 1, 1), 1.0f), "R maps to stereo R");
    require(near(gainAt(fiveOneToStereo, 0, 2), ChannelRouter::minusThreeDbGain()), "C maps to L at -3 dB");
    require(near(gainAt(fiveOneToStereo, 1, 2), ChannelRouter::minusThreeDbGain()), "C maps to R at -3 dB");
    require(near(gainAt(fiveOneToStereo, 0, 3), 0.0f), "LFE is silent by default");
    require(juce::String(ChannelRouter::downmixProfileId()) == "wwise-ac3-preview-v1",
            "downmix coefficients expose a locked profile identity");
    struct DownmixReference
    {
        int outputChannel;
        int sourceChannel;
        float expectedGain;
    };
    const std::array<DownmixReference, 8> ac3StereoReferences {{
        { 0, 0, 1.0f },
        { 1, 1, 1.0f },
        { 0, 2, ChannelRouter::minusThreeDbGain() },
        { 1, 2, ChannelRouter::minusThreeDbGain() },
        { 0, 3, 0.0f },
        { 1, 3, 0.0f },
        { 0, 4, ChannelRouter::minusThreeDbGain() },
        { 1, 5, ChannelRouter::minusThreeDbGain() }
    }};
    for (const auto& reference : ac3StereoReferences)
        require(near(gainAt(fiveOneToStereo,
                            reference.outputChannel,
                            reference.sourceChannel),
                     reference.expectedGain),
                "Wwise/AC-3 stereo downmix reference vector matches");

    const auto fiveOneToSevenOne = ChannelRouter::buildRoutingMatrix(fiveOne,
                                                                     ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(8),
                                                                     ChannelPlaybackState {});
    require(near(gainAt(fiveOneToSevenOne, 4, 4), 1.0f), "5.1 Ls routes to 7.1 side left");
    require(near(gainAt(fiveOneToSevenOne, 5, 5), 1.0f), "5.1 Rs routes to 7.1 side right");
    require(near(gainAt(fiveOneToSevenOne, 0, 4), 0.0f), "5.1 Ls does not fall back to front left");
    require(near(gainAt(fiveOneToSevenOne, 1, 5), 0.0f), "5.1 Rs does not fall back to front right");

    const auto sevenOneToFiveOne = ChannelRouter::buildRoutingMatrix(
        smpteWithMaskCandidate,
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(6),
        ChannelPlaybackState {});
    require(near(gainAt(sevenOneToFiveOne, 4, 4), 1.0f),
            "Wwise 7.1 side-left maps to 5.1 surround-left at unity");
    require(near(gainAt(sevenOneToFiveOne, 4, 6), 1.0f),
            "Wwise 7.1 rear-left folds to 5.1 surround-left at unity");
    require(near(gainAt(sevenOneToFiveOne, 5, 7), 1.0f),
            "Wwise 7.1 rear-right folds to 5.1 surround-right at unity");

    ChannelPlaybackState patchedSevenOneState;
    require(patchedSevenOneState.outputMapping.setOutputChannel(SpeakerRole::Lss, 6),
            "output mapping accepts side-left to output 7");
    require(patchedSevenOneState.outputMapping.setOutputChannel(SpeakerRole::Rss, 7),
            "output mapping accepts side-right to output 8");
    require(patchedSevenOneState.outputMapping.setOutputChannel(SpeakerRole::Lsr, 4),
            "output mapping accepts rear-left to output 5");
    require(patchedSevenOneState.outputMapping.setOutputChannel(SpeakerRole::Rsr, 5),
            "output mapping accepts rear-right to output 6");
    const auto patchedSevenOne = ChannelRouter::buildRoutingMatrix(
        ChannelLayoutCatalog::layoutForPresetId(8, "7.1-smpte"),
        deviceSevenOne,
        patchedSevenOneState);
    require(near(gainAt(patchedSevenOne, 6, 4), 1.0f),
            "output mapping routes semantic side-left to physical output 7");
    require(near(gainAt(patchedSevenOne, 4, 6), 1.0f),
            "output mapping routes semantic rear-left to physical output 5");
    require(ChannelRouter::outputMappingIsValid(deviceSevenOne,
                                                 patchedSevenOneState.outputMapping),
            "side/rear permutation is a valid output mapping");
    const auto physicalMatrix = ChannelRouter::buildPhysicalOutputMatrix(
        deviceSevenOne,
        patchedSevenOneState.outputMapping);
    require(physicalMatrix.valid && physicalMatrix.activeOutputChannels == 8
                && physicalMatrix.routes.size() == 8,
            "physical output matrix adapts to the active eight-channel device");
    require(ChannelRouter::buildPhysicalOutputMatrix(
                ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(2), {})
                    .activeOutputChannels == 2
                && ChannelRouter::buildPhysicalOutputMatrix(
                       ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(6), {})
                       .activeOutputChannels == 6
                && ChannelRouter::buildPhysicalOutputMatrix(
                       ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12), {})
                       .activeOutputChannels == 12,
            "physical matrix adapts deterministically to stereo, 5.1, and 7.1.4 devices");
    require(physicalMatrix.routes[4].role == SpeakerRole::Lss
                && physicalMatrix.routes[4].canonicalChannel == 4
                && physicalMatrix.routes[4].physicalOutputChannel == 6,
            "physical matrix keeps canonical side-left separate from output 7");
    const auto filmSource = ChannelLayoutCatalog::layoutForPresetId(8, "7.1-film-protools");
    const auto physicalMatrixAfterSourceChange = ChannelRouter::buildPhysicalOutputMatrix(
        deviceSevenOne,
        patchedSevenOneState.outputMapping);
    require(filmSource.selectedRoles != smpteWithMaskCandidate.selectedRoles
                && physicalMatrixAfterSourceChange.routes[4].physicalOutputChannel
                       == physicalMatrix.routes[4].physicalOutputChannel,
            "changing source interpretation never rewires physical device order");
    const auto silentPings = ChannelRouter::buildSilentRouteProbes(
        ChannelLayoutCatalog::layoutForPresetId(8, "7.1-smpte"),
        physicalMatrix,
        patchedSevenOne);
    require(silentPings.size() == 8 && silentPings[4].sourceRole == SpeakerRole::Lss
                && silentPings[4].routed && silentPings[4].finite
                && silentPings[4].outputs.size() == 1
                && silentPings[4].outputs[0].targetRole == SpeakerRole::Lss
                && silentPings[4].outputs[0].physicalOutputChannel == 6
                && near(silentPings[4].outputs[0].gain, 1.0f),
            "silent per-role ping proves source side-left to role to physical output 7");

    auto duplicateMapping = patchedSevenOneState.outputMapping;
    require(duplicateMapping.setOutputChannel(SpeakerRole::Lsr, 6),
            "mapping model accepts data before target-aware validation");
    require(!ChannelRouter::outputMappingIsValid(deviceSevenOne, duplicateMapping),
            "duplicate physical output assignment is rejected by validation");

    const auto sevenOneFourToSevenOne = ChannelRouter::buildRoutingMatrix(
        sevenOneFour,
        deviceSevenOne,
        ChannelPlaybackState {});
    require(near(gainAt(sevenOneFourToSevenOne, 0, 8), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-front-left folds to front-left at -3 dB");
    require(near(gainAt(sevenOneFourToSevenOne, 1, 9), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-front-right folds to front-right at -3 dB");
    require(near(gainAt(sevenOneFourToSevenOne, 6, 10), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-rear-left folds to rear-left at Wwise -3 dB");
    require(near(gainAt(sevenOneFourToSevenOne, 7, 11), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-rear-right folds to rear-right at Wwise -3 dB");
    require(near(gainAt(sevenOneFourToSevenOne, 0, 10), 0.0f)
                && near(gainAt(sevenOneFourToSevenOne, 6, 8), 0.0f),
            "height channels do not leak between front and rear destinations");

    const auto sevenOneFourToFiveOne = ChannelRouter::buildRoutingMatrix(
        sevenOneFour,
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(6),
        ChannelPlaybackState {});
    require(near(gainAt(sevenOneFourToFiveOne, 0, 8), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-front-left folds to 5.1 front-left at -3 dB");
    require(near(gainAt(sevenOneFourToFiveOne, 4, 10), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-rear-left folds to 5.1 surround-left at Wwise -3 dB");
    require(near(gainAt(sevenOneFourToFiveOne, 5, 11), ChannelRouter::minusThreeDbGain()),
            "7.1.4 top-rear-right folds to 5.1 surround-right at Wwise -3 dB");

    require(ChannelLayoutCatalog::roleFromAbbreviation("Lss") == SpeakerRole::Lss,
            "speaker role abbreviation parses for CLI output mapping");
    require(ChannelLayoutCatalog::roleFromAbbreviation("not-a-role") == SpeakerRole::unknown,
            "unknown speaker role abbreviation is rejected");

    const auto monoTarget = ChannelLayoutCatalog::monoOutputLayout();
    const auto fiveOneToMono = ChannelRouter::buildRoutingMatrix(fiveOne,
                                                                 monoTarget,
                                                                 ChannelPlaybackState {});
    require(fiveOneToMono.outputChannels == 1, "mono target has one output");
    require(near(gainAt(fiveOneToMono, 0, 0), ChannelRouter::minusThreeDbGain()),
            "Wwise front-left maps to mono at -3 dB");
    require(near(gainAt(fiveOneToMono, 0, 1), ChannelRouter::minusThreeDbGain()),
            "Wwise front-right maps to mono at -3 dB");
    require(near(gainAt(fiveOneToMono, 0, 2), 1.0f),
            "Wwise center maps to mono at unity without double accumulation");
    require(near(gainAt(fiveOneToMono, 0, 4), ChannelRouter::minusSixDbGain()),
            "Wwise surround-left maps to mono at -6 dB");

    const auto sevenOneFourToMono = ChannelRouter::buildRoutingMatrix(
        sevenOneFour,
        monoTarget,
        ChannelPlaybackState {});
    require(near(gainAt(sevenOneFourToMono, 0, 8), ChannelRouter::minusSixDbGain()),
            "Wwise top-front-left maps to mono at -6 dB");
    require(near(gainAt(sevenOneFourToMono, 0, 10), ChannelRouter::minusNineDbGain()),
            "Wwise top-rear-left maps to mono at -9 dB");

    ChannelPlaybackState includeLfe;
    includeLfe.includeLfeInDownmix = true;
    const auto fiveOneWithLfe = ChannelRouter::buildRoutingMatrix(fiveOne, stereoTarget, includeLfe);
    require(gainAt(fiveOneWithLfe, 0, 3) > 0.0f, "Include LFE sends LFE to L");
    require(gainAt(fiveOneWithLfe, 1, 3) > 0.0f, "Include LFE sends LFE to R");

    ChannelPlaybackState muteCenter;
    muteCenter.mutedBySourceChannel[2] = true;
    const auto fiveOneMuteCenter = ChannelRouter::buildRoutingMatrix(fiveOne, stereoTarget, muteCenter);
    require(near(gainAt(fiveOneMuteCenter, 0, 2), 0.0f), "Muted center does not route to L");
    require(near(gainAt(fiveOneMuteCenter, 1, 2), 0.0f), "Muted center does not route to R");

    ChannelPlaybackState soloSurround;
    soloSurround.soloedBySourceChannel[4] = true;
    const auto fiveOneSoloSurround = ChannelRouter::buildRoutingMatrix(fiveOne, stereoTarget, soloSurround);
    require(near(gainAt(fiveOneSoloSurround, 0, 0), 0.0f), "Solo mutes non-solo L");
    require(near(gainAt(fiveOneSoloSurround, 0, 4), ChannelRouter::minusThreeDbGain()), "Solo keeps selected surround");

    const auto sevenOneTarget = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(8);
    const auto folded = ChannelRouter::buildRoutingMatrix(sevenOneFour,
                                                          sevenOneTarget,
                                                          ChannelPlaybackState {});
    require(folded.outputChannels == 8, "7.1.4 to 7.1 output count");
    require(near(gainAt(folded, 0, 8), ChannelRouter::minusThreeDbGain()), "Ltf folds to L");
    require(near(gainAt(folded, 1, 9), ChannelRouter::minusThreeDbGain()), "Rtf folds to R");
    require(near(gainAt(folded, 6, 10), ChannelRouter::minusThreeDbGain()),
            "Ltr folds to Lsr at Wwise -3 dB");
    require(near(gainAt(folded, 7, 11), ChannelRouter::minusThreeDbGain()),
            "Rtr folds to Rsr at Wwise -3 dB");

    runAmbisonicBinauralFilterBankTest();
    runAmbisonicBinauralRendererTest();
    runAmbisonicSpeakerGeometryTest();
    runOutputMeterAnalyzerTest();
    runAmbisonicFieldAnalyzerTest();
    runAmbiXMetadataDetectionTest();
    runDocumentSaveOverwriteTest();
    runDocumentBinauralModeTest();
    runWorkspaceModuleLayoutTest();

    std::cout << "ChannelRoutingTests passed\n";
    return 0;
}
