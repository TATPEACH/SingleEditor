#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singleeditor
{
// D111/D112. One channel is one FINAL physical output slot, after routing and
// summation. Direction/role resolution belongs to the immutable control-side
// layout, never to the callback or an inferred source-channel count.
class SpatialEnergyMeter final
{
public:
    static constexpr int maxChannels = 32; // D105 active-bus limit; physical indices may be noncontiguous
    static constexpr std::size_t queueCapacity = 128; // 6.4 s at 20 Hz
    enum class Transport { running, paused, stopped, unavailable };
    enum class Placement { directional, lfe, unplaced };
    struct Channel
    {
        int physicalOutput = -1;
        Placement placement = Placement::unplaced;
        float azimuthDegrees = 0, elevationDegrees = 0;
        // BS.1770 weight from trusted output semantics. Unknown is NOT unity.
        // LFE always has zero global weight, but retains its own diagnostics.
        std::optional<double> loudnessWeight;
        std::string label, speakerId;
        bool mapped = false, ambiguous = false;
    };
    struct Layout
    {
        std::uint64_t generation = 0;
        double sampleRate = 0;
        std::vector<Channel> channels; // exact final device-buffer slot order
        int unmappedSpeakerCount = 0;
        bool geometryCurrent = false;
    };
    struct ChannelValue
    {
        double kWeightedMeanSquare = 0; // unweighted by speaker position
        double contributionDb = -INFINITY; // 10 log10(K power), NOT channel LUFS
        double rmsDbfs = -INFINITY;
        double samplePeakDbfs = -INFINITY; // explicitly not a true-peak estimate
    };
    struct Snapshot
    {
        std::shared_ptr<const Layout> layout;
        std::vector<ChannelValue> channels;
        Transport transport = Transport::stopped;
        std::uint64_t epoch = 0, sequence = 0, measuredSamples = 0;
        std::uint64_t integratedThroughSamples = 0, droppedBlocks = 0;
        bool momentaryReady = false, shortTermReady = false;
        bool discontinuity = false; // lost measurement data; never hidden as UI coalescing
        std::optional<double> momentaryLufs, shortTermLufs, integratedLufs;
        std::optional<double> maximumMomentaryLufs, maximumShortTermLufs;
    };

    // Construct/destroy on the owning worker, with no callback using the object.
    // No thread, timer, producer or filter exists until its owner acquires a lease.
    explicit SpatialEnergyMeter(Layout);
    ~SpatialEnergyMeter();
    SpatialEnergyMeter(const SpatialEnergyMeter&) = delete;
    SpatialEnergyMeter& operator=(const SpatialEnergyMeter&) = delete;

    // Single audio writer. Fixed storage, no allocation/lock. epoch changes on
    // Seek/source/route/geometry/binding/device changes, compared by identity only.
    void process(const juce::AudioBuffer<float>& finalOutputs, int start, int samples,
                 std::uint64_t epoch, Transport) noexcept;
    // Single worker reader (~20 Hz). Drains all measurement records, returns
    // only the newest immutable result. A slow UI cannot discard gate history.
    std::shared_ptr<const Snapshot> poll();
    std::uint64_t droppedBlockCount() const noexcept { return dropped.load(std::memory_order_relaxed); }
    static double powerDb(double power) noexcept;

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process(double x) noexcept;
        void reset() noexcept { z1 = z2 = 0; }
    };
    struct Block
    {
        std::array<double, maxChannels> energy {}, rawEnergy {};
        std::array<float, maxChannels> peak {};
        std::uint64_t serial = 0, epoch = 0, run = 0, endSample = 0, droppedBefore = 0;
        int samples = 0;
        Transport transport = Transport::stopped;
    };
    void resetAudio(std::uint64_t) noexcept;
    void push(Block&) noexcept;
    void resetWorker(const Block&, bool gap);
    void consume(const Block&);
    void updateIntegrated();
    void updateSnapshot();

    std::shared_ptr<const Layout> layout;
    std::array<Biquad, maxChannels> shelf, highPass;
    std::vector<Block> queue, history;
    std::atomic<std::uint64_t> written { 0 };
    std::atomic<std::uint64_t> read { 0 };
    std::atomic<std::uint64_t> dropped { 0 };
    Block partial;
    std::uint64_t audioEpoch = 0, audioRun = 0, audioSamples = 0, tick = 0, nextEnd = 0, serial = 0;
    Transport audioTransport = Transport::unavailable;
    bool audioInitialised = false, restartAfterStop = false;
    Snapshot current;
    std::size_t historyWrite = 0, historyCount = 0;
    std::array<double, maxChannels> momentaryEnergy {}, momentaryRaw {}, shortEnergy {};
    std::uint64_t momentarySamples = 0, shortSamples = 0, workerRun = 0, workerSerial = 0, blocks = 0;
    std::uint64_t acknowledgedDrops = 0, lastGateEndSample = 0;
    std::vector<double> gateEnergies; // worker only: 80 bytes/s of admitted programme
    double absoluteGateSum = 0;
    bool weightsKnown = true;
};
}
