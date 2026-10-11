#include "Audio/SpatialEnergyMeter.h"
#include "Audio/SpatialEnergyLayout.h"
#include "Audio/DeviceRoutePlan.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 // SDK legacy qualifiers must not replace existing test helper names.
 #undef near
 #undef far
 #undef small
#endif

namespace
{
using Meter = singleeditor::SpatialEnergyMeter;
using Plan = singleeditor::DeviceRoutePlan;
using Role = singleeditor::SpeakerRole;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void near(double value, double expected, double tolerance, const char* message)
{
    if (std::abs(value - expected) > tolerance || !std::isfinite(value))
    {
        std::cerr << message << ": got " << value << ", expected " << expected << '\n';
        throw std::runtime_error(message);
    }
}
Meter::Layout stereo(double rate = 48000)
{
    return { 7, rate, {{1, Meter::Placement::directional, -30, 0, 1.0},
                      {3, Meter::Placement::directional, 30, 0, 1.0}}, 0, true };
}
juce::int64 privateMemoryBytes()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX values {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&values),sizeof(values)))
        return static_cast<juce::int64>(values.PrivateUsage);
   #endif
    return -1;
}
void programmeResourceProbe(const juce::File& receipt)
{
    constexpr int rate=48000, blockSize=4800, programmeBlocks=72000;
    auto meter=std::make_unique<Meter>(stereo(rate));
    juce::AudioBuffer<float> loud(2,blockSize), quiet(2,blockSize);
    for(int c=0;c<2;++c) for(int i=0;i<blockSize;++i)
    {
        const auto wave=std::sin(juce::MathConstants<double>::twoPi*1000*i/rate);
        loud.setSample(c,i,static_cast<float>(wave*std::pow(10.0,-23.0/20)));
        quiet.setSample(c,i,static_cast<float>(wave*std::pow(10.0,-35.0/20)));
    }
    const auto before=privateMemoryBytes();
    double processTotal=0,processMaximum=0,pollTotal=0,pollMaximum=0;
    const auto started=juce::Time::getMillisecondCounterHiRes();
    std::shared_ptr<const Meter::Snapshot> snapshot;
    for(int i=0;i<programmeBlocks;++i)
    {
        const auto t0=juce::Time::getMillisecondCounterHiRes();
        meter->process(i<programmeBlocks/2 ? loud : quiet,0,blockSize,91,Meter::Transport::running);
        const auto t1=juce::Time::getMillisecondCounterHiRes();
        snapshot=meter->poll();
        const auto t2=juce::Time::getMillisecondCounterHiRes();
        processTotal+=t1-t0; processMaximum=std::max(processMaximum,t1-t0);
        pollTotal+=t2-t1; pollMaximum=std::max(pollMaximum,t2-t1);
        require(snapshot && !snapshot->discontinuity,"long programme retains every measurement block");
        if(i==programmeBlocks/2-1)
        {
            require(snapshot->integratedLufs.has_value(),"first hour has an integrated value");
            near(*snapshot->integratedLufs,-23,0.05,"first hour calibration");
        }
    }
    const auto elapsed=juce::Time::getMillisecondCounterHiRes()-started;
    const auto expected=10*std::log10((std::pow(10.0,-23.0/10)+std::pow(10.0,-35.0/10))/2);
    require(snapshot->integratedLufs.has_value(),"long programme has complete integrated loudness");
    near(*snapshot->integratedLufs,expected,0.05,"second hour retains first-hour programme in exact gate history");
    require(snapshot->measuredSamples==static_cast<std::uint64_t>(programmeBlocks)*blockSize
        && snapshot->integratedThroughSamples==snapshot->measuredSamples && snapshot->droppedBlocks==0,
        "two-hour actual processed timeline has complete gates and no drops");
    const auto after=privateMemoryBytes();
    juce::DynamicObject::Ptr report=new juce::DynamicObject;
    report->setProperty("schema","singleeditor.spatial-energy.programme-resources.v1");
    report->setProperty("scope","instrumented offline measurement; not CoreReady, hardware deadline or complete process allocation proof");
    report->setProperty("sampleRate",rate); report->setProperty("physicalChannels",2);
    report->setProperty("logicalSeconds",7200); report->setProperty("blockSamples",blockSize);
    report->setProperty("processedSamples",static_cast<juce::int64>(snapshot->measuredSamples));
    report->setProperty("integratedLufs",*snapshot->integratedLufs); report->setProperty("independentExpectedLufs",expected);
    report->setProperty("elapsedMs",elapsed);
    report->setProperty("processMeanUs",processTotal*1000/programmeBlocks); report->setProperty("processMaxUs",processMaximum*1000);
    report->setProperty("pollMeanUs",pollTotal*1000/programmeBlocks); report->setProperty("pollMaxUs",pollMaximum*1000);
    report->setProperty("privateBytesBefore",before); report->setProperty("privateBytesAfter",after);
    snapshot.reset(); meter.reset(); report->setProperty("privateBytesAfterDestroy",privateMemoryBytes());
    require(receipt.replaceWithText(juce::JSON::toString(juce::var(report.get()))),"write programme resource evidence");
    std::cout<<"PASS: real two-hour stereo programme, retained first-hour history, complete gate timeline; receipt="<<receipt.getFullPathName()<<'\n';
}
struct Signal
{
    explicit Signal(Meter::Layout l) : layout(std::move(l)), meter(layout), buffer(static_cast<int>(layout.channels.size()), 4096) {}
    void tone(double seconds, const std::vector<double>& levels, int blockSize = 257, bool drain = true)
    {
        auto remaining = static_cast<int>(std::llround(seconds * layout.sampleRate));
        while (remaining > 0)
        {
            const auto n = std::min(blockSize, remaining);
            for (int c = 0; c < buffer.getNumChannels(); ++c)
                for (int s = 0; s < n; ++s)
                    buffer.setSample(c, s, static_cast<float>(std::pow(10.0, levels[static_cast<std::size_t>(c)] / 20.0)
                        * std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * static_cast<double>(position + s) / layout.sampleRate)));
            meter.process(buffer, 0, n, epoch, Meter::Transport::running);
            position += n;
            remaining -= n;
            if (drain) poll();
        }
    }
    void state(Meter::Transport transport) { meter.process(buffer, 0, 0, epoch, transport); poll(); }
    void poll() { if (auto next = meter.poll()) snapshot = std::move(next); }
    Meter::Layout layout;
    Meter meter;
    juce::AudioBuffer<float> buffer;
    std::int64_t position = 0;
    std::uint64_t epoch = 90;
    std::shared_ptr<const Meter::Snapshot> snapshot;
};

void calibrationAndGates()
{
    for (const auto rate : {44100.0, 48000.0, 96000.0})
    {
        Signal signal(stereo(rate));
        signal.tone(20, {-23, -23});
        require(signal.snapshot && signal.snapshot->momentaryReady && signal.snapshot->shortTermReady, "complete M/S windows");
        near(*signal.snapshot->momentaryLufs, -23, .1, "EBU stereo calibration M");
        near(*signal.snapshot->shortTermLufs, -23, .1, "EBU stereo calibration S");
        near(*signal.snapshot->integratedLufs, -23, .1, "EBU stereo calibration I");
        near(signal.snapshot->channels[0].samplePeakDbfs, -23, .03, "sample peak labelled dBFS");
        near(signal.snapshot->channels[0].rmsDbfs, -26.0102999566, .002, "raw RMS separate from K energy");
        require(signal.snapshot->measuredSamples == static_cast<std::uint64_t>(rate * 20), "actual sample clock has no callback drift");
    }
    Signal gated(stereo());
    gated.tone(10, {-72, -72}); gated.tone(10, {-36, -36}); gated.tone(60, {-23, -23});
    gated.tone(10, {-36, -36}); gated.tone(10, {-72, -72}); gated.state(Meter::Transport::stopped);
    near(*gated.snapshot->integratedLufs, -23, .1, "EBU absolute and relative gates");
    Signal relative(stereo());
    relative.tone(20, {-26, -26}); relative.tone(20.1, {-20, -20}); relative.tone(20, {-26, -26});
    relative.state(Meter::Transport::paused);
    near(*relative.snapshot->integratedLufs, -23, .1, "EBU relative gate recalculates all admitted history");
    Signal silent(stereo()); silent.tone(3, {-INFINITY, -INFINITY});
    require(silent.snapshot->integratedLufs && std::isinf(*silent.snapshot->integratedLufs)
        && *silent.snapshot->integratedLufs < 0, "known silence is negative infinity, not unknown or a display floor");
    std::cout << "PASS: 44.1/48/96 kHz calibration, exact gates and silence\n";
}

void windowsAndTransport()
{
    Signal small(stereo()), large(stereo());
    small.tone(3.5, {-29, -17}, 7); large.tone(3.5, {-29, -17}, 4096);
    near(*small.snapshot->momentaryLufs, *large.snapshot->momentaryLufs, 1e-10, "M independent of callback partition");
    near(*small.snapshot->shortTermLufs, *large.snapshot->shortTermLufs, 1e-10, "S independent of callback partition");
    near(*small.snapshot->integratedLufs, *large.snapshot->integratedLufs, 1e-10, "I independent of callback partition");
    const auto trusted = small.snapshot;
    small.state(Meter::Transport::paused);
    small.meter.process(small.buffer, 0, 1000, small.epoch, Meter::Transport::paused); small.poll();
    require(small.snapshot->measuredSamples == trusted->measuredSamples
        && small.snapshot->momentaryLufs == trusted->momentaryLufs, "pause retains trusted values, ignores callback silence");
    small.tone(.05, {-29, -17}, 73); small.state(Meter::Transport::paused);
    require(small.snapshot->integratedThroughSamples == 168000 && small.snapshot->measuredSamples == 170400,
        "incomplete 400 ms gate at pause not included in I timestamp");
    small.state(Meter::Transport::stopped);
    require(small.snapshot->momentaryLufs.has_value(), "stop preserves actual values for display-only decay");
    small.tone(.1, {-23, -23});
    require(!small.snapshot->momentaryLufs && !small.snapshot->shortTermLufs && !small.snapshot->integratedLufs,
        "new run after stop resets all windows");
    small.tone(.3, {-23, -23}); require(small.snapshot->momentaryReady && !small.snapshot->shortTermReady, "M ready exactly at 400 ms");
    small.epoch = 2; // identity changed backwards, NOT a chronological numeric version
    small.state(Meter::Transport::paused);
    require(!small.snapshot->momentaryReady && !small.snapshot->integratedLufs && small.snapshot->epoch == 2,
        "backwards epoch identity cuts history even while paused");
    small.tone(3, {-33, -33});
    near(*small.snapshot->integratedLufs, -33, .1, "Seek starts a clean integration");
    require(trusted->measuredSamples == 168000, "published snapshots remain immutable after reset");
    std::cout << "PASS: callback partitions, 400 ms/3 s, Pause/Stop/Seek and immutable snapshots\n";
}

// Independent direct-form recurrence, using the published ITU 48 kHz table,
// not the production filter generator or chunk/window implementation.
struct ReferenceFilter
{
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double apply(double x, double b0, double b1, double b2, double a1, double a2)
    {
        const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};
void referenceNoiseAndImpulse()
{
    for (const auto impulse : {false, true})
    {
        Signal signal(stereo());
        std::array<ReferenceFilter, 2> shelves, highPass;
        std::array<double, 2> m {}, s {};
        std::uint32_t random = 871;
        constexpr int total = 163200; // 3.4 s
        for (int offset = 0; offset < total;)
        {
            const auto n = std::min(379, total - offset);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                    const auto value = impulse ? (offset + i == total - 15000 ? .5f : 0.0f)
                        : static_cast<float>((static_cast<double>(random) / 4294967295.0 - .5) * .2);
                    signal.buffer.setSample(c, i, value);
                    const auto k = highPass[static_cast<std::size_t>(c)].apply(
                        shelves[static_cast<std::size_t>(c)].apply(value, 1.53512485958697, -2.69169618940638,
                            1.19839281085285, -1.69065929318241, .73248077421585),
                        1, -2, 1, -1.99004745483398, .99007225036621);
                    if (offset + i >= total - 19200) m[static_cast<std::size_t>(c)] += k * k;
                    if (offset + i >= total - 144000) s[static_cast<std::size_t>(c)] += k * k;
                }
            signal.meter.process(signal.buffer, 0, n, 19, Meter::Transport::running);
            signal.poll(); offset += n;
        }
        for (std::size_t c = 0; c < 2; ++c)
            near(signal.snapshot->channels[c].contributionDb, Meter::powerDb(m[c] / 19200.0), .0001,
                "noise/impulse unweighted K contribution matches independent recurrence and exact window");
        near(*signal.snapshot->momentaryLufs, -.691 + Meter::powerDb((m[0] + m[1]) / 19200.0), .0001, "reference noise/impulse M");
        near(*signal.snapshot->shortTermLufs, -.691 + Meter::powerDb((s[0] + s[1]) / 144000.0), .0001, "reference noise/impulse S");
    }
    std::cout << "PASS: independent ITU-coefficient noise and impulse window oracle\n";
}

void physicalRoute()
{
    Plan::Bus bus {"test", 4, {{"left", Role::L}, {"right", Role::R}, {"sub", Role::LFE}}};
    singleeditor::DeviceOutput::Snapshot device {"test", "device", "test", 48000, 512, 32, {1, 3, 6},
        {"0", "1", "2", "3", "4", "5", "6"}, {}, {}};
    auto layout = stereo(); layout.channels.push_back({6, Meter::Placement::lfe, 0, 0, {}});
    std::vector<double> global, channel;
    for (int mode = 0; mode < 3; ++mode)
    {
        auto draft = Plan::propose(bus, device);
        draft.outputsBySpeaker = mode == 0 ? std::vector<std::vector<int>>{{1}, {}, {6}}
            : mode == 1 ? std::vector<std::vector<int>>{{1}, {1}, {6}}
                        : std::vector<std::vector<int>>{{1, 3}, {}, {6}};
        const auto plan = Plan::validate(draft, device, bus).approved(true);
        require(plan != nullptr, "explicit route reviewed");
        Signal measured(layout);
        juce::AudioBuffer<float> source(3, 512);
        for (int block = 0; block < 300; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const auto x = static_cast<float>(.1 * std::sin(2 * juce::MathConstants<double>::pi * 1000 * (block * 512 + i) / 48000.0));
                source.setSample(0, i, x); source.setSample(1, i, x); source.setSample(2, i, x * 8);
            }
            require(plan->render(source.getArrayOfReadPointers(), 3, measured.buffer.getArrayOfWritePointers(), 3, 512), "render final physical outputs");
            measured.meter.process(measured.buffer, 0, 512, 1, Meter::Transport::running); measured.poll();
        }
        global.push_back(*measured.snapshot->momentaryLufs);
        channel.push_back(measured.snapshot->channels[0].contributionDb);
        near(measured.snapshot->channels[2].rmsDbfs, Meter::powerDb(.8 * .8 / 2), .02, "LFE diagnostic retained independently");
    }
    near(global[1] - global[0], 6.020599913, .0001, "many-to-one measures summed amplitude once, not sum of source powers");
    near(channel[1] - channel[0], 6.020599913, .0001, "merged physical contribution only once");
    near(global[2] - global[0], 3.010299956, .0001, "copies are two real physical outputs");
    near(channel[2], channel[0], .0001, "copy does not change physical amplitude");
    auto unknown = stereo(); unknown.channels[1].placement = Meter::Placement::unplaced; unknown.channels[1].loudnessWeight.reset();
    unknown.geometryCurrent = false; unknown.unmappedSpeakerCount = 1;
    Signal unplaced(unknown); unplaced.tone(3, {-23, -23});
    require(!unplaced.snapshot->momentaryLufs && !unplaced.snapshot->integratedLufs
        && std::isfinite(unplaced.snapshot->channels[1].contributionDb)
        && unplaced.snapshot->layout->unmappedSpeakerCount == 1 && !unplaced.snapshot->layout->geometryCurrent,
        "unknown output semantics preserves discrete measurement, never invents global LUFS or geometry");
    auto duplicate = stereo(); duplicate.channels[1].physicalOutput = 1;
    bool rejected = false; try { Meter invalid(duplicate); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "same physical channel cannot be counted twice in descriptor");
    std::cout << "PASS: real route sum/copy/unmapped, LFE, unplaced and unique physical identity\n";
}

void multichannel()
{
    auto layout = stereo();
    layout.channels = {{0, Meter::Placement::directional, -30, 0, 1}, {1, Meter::Placement::directional, 30, 0, 1},
        {2, Meter::Placement::directional, 0, 0, 1}, {3, Meter::Placement::lfe, 0, 0, {}},
        {4, Meter::Placement::directional, -110, 0, 1.41}, {5, Meter::Placement::directional, 110, 0, 1.41}};
    Signal surround(layout); surround.tone(20, {-28, -28, -24, 0, -30, -30});
    near(*surround.snapshot->integratedLufs, -23, .1, "EBU 5.0 calibration plus loud excluded LFE");
    // Equal per-channel signal: position weight affects global LUFS, never the field contribution.
    Signal equal(layout); equal.tone(3, {-23, -23, -23, -23, -23, -23});
    near(equal.snapshot->channels[0].contributionDb, equal.snapshot->channels[4].contributionDb, 1e-10,
        "field K contributions do not inherit BS.1770 surround gain");
    std::cout << "PASS: 5.1 global position weights versus unweighted spatial contributions\n";
}

void backlogAndConcurrency()
{
    Signal overflow(stereo()); overflow.tone(8, {-23, -23}, 512, false); overflow.poll();
    require(overflow.meter.droppedBlockCount() > 0 && overflow.snapshot->discontinuity, "bounded callback queue reports measurement loss");
    overflow.tone(.4, {-33, -33});
    require(overflow.snapshot->discontinuity && !overflow.snapshot->shortTermReady, "post-gap history cannot mix with old segment");
    overflow.epoch = 3; overflow.tone(.4, {-33, -33});
    require(!overflow.snapshot->discontinuity && overflow.snapshot->momentaryReady, "explicit new epoch clears old discontinuity");

    Signal concurrent(stereo());
    std::atomic<bool> done {false};
    std::thread producer([&] { concurrent.tone(6, {-23, -23}, 512, false); done.store(true, std::memory_order_release); });
    while (!done.load(std::memory_order_acquire)) { concurrent.poll(); std::this_thread::yield(); }
    producer.join(); concurrent.poll();
    require(concurrent.snapshot && !concurrent.snapshot->discontinuity, "single audio writer/worker reader keeps complete history");
    near(*concurrent.snapshot->integratedLufs, -23, .1, "threaded queue publishes coherent final measurement");
    std::cout << "PASS: queue overflow is explicit; concurrent SPSC drain and latest immutable publication\n";
}

void geometryBindings()
{
    using Builder = singleeditor::SpatialEnergyLayout;
    singleeditor::SpeakerGeometryProfile geometry;
    geometry.profileId = "spatial-test"; geometry.revision = 8;
    const std::vector<Role> roles {Role::L, Role::R, Role::C, Role::LFE, Role::Lss, Role::Rss,
        Role::Lsr, Role::Rsr, Role::Ltf, Role::Rtf, Role::Ltr, Role::Rtr};
    const std::vector<float> azimuth {-30, 30, 0, 0, -90, 90, -150, 150, -45, 45, -135, 135};
    for (const auto channels : {2, 6, 8, 12})
    {
        geometry.speakers.clear();
        Plan::Bus bus {"spatial-test", 8, {}};
        singleeditor::DeviceOutput::Snapshot device {"ASIO", "exact", "fixture", 48000, 512, 32, {}, {}, {}, {}};
        for (int c = 0; c < channels; ++c)
        {
            singleeditor::SpeakerGeometrySpeaker speaker;
            speaker.stableId = "speaker-" + juce::String(c); speaker.label = speaker.stableId;
            speaker.role = roles[static_cast<std::size_t>(c)]; speaker.azimuthDegrees = azimuth[static_cast<std::size_t>(c)];
            speaker.elevationDegrees = c >= 8 ? 45.0f : 0.0f;
            geometry.speakers.push_back(speaker);
            bus.speakers.push_back({speaker.stableId.toStdString(), speaker.role, true});
            device.physicalOutputs.push_back(c * 2 + 1);
            device.outputNames.push_back("inactive"); device.outputNames.push_back("Port " + std::to_string(c));
        }
        auto draft = Plan::propose(bus, device);
        for (int c = 0; c < channels; ++c) draft.outputsBySpeaker[static_cast<std::size_t>(c)] = {c * 2 + 1};
        const auto route = Plan::validate(draft, device, bus).approved(true);
        require(route != nullptr, "geometry binding route approved");
        auto layout = Builder::fromRoute(*route, &geometry, 81);
        require(layout.geometryCurrent && layout.generation == 81 && layout.unmappedSpeakerCount == 0, "exact profile revision and bus bound");
        for (int c = 0; c < channels; ++c)
        {
            const auto& channel = layout.channels[static_cast<std::size_t>(c)];
            require(channel.physicalOutput == c * 2 + 1 && channel.mapped && !channel.ambiguous, "physical index is not compact buffer index");
            near(*channel.loudnessWeight, c == 3 ? 0.0 : (c == 4 || c == 5 ? 1.41 : 1.0), 1e-12,
                "standard geometry rear and height weights remain unity, lateral pair 1.41");
        }
        Signal measured(layout);
        measured.tone(3, std::vector<double>(static_cast<std::size_t>(channels), -23));
        const auto sumWeights = channels == 2 ? 2.0 : static_cast<double>(channels - 1) + .82;
        near(*measured.snapshot->momentaryLufs, -23 + 10 * std::log10(sumWeights / 2), .1, "Stereo/5.1/7.1/7.1.4 physical loudness");

        ++geometry.revision;
        layout = Builder::fromRoute(*route, &geometry, 82);
        require(!layout.geometryCurrent && std::all_of(layout.channels.begin(), layout.channels.end(), [](const auto& c)
            { return c.placement == Meter::Placement::unplaced && !c.loudnessWeight; }), "stale geometry never supplies guessed directions or weights");
        --geometry.revision;
        draft.outputsBySpeaker[1] = {1};
        const auto merged = Plan::validate(draft, device, bus).approved(true);
        layout = Builder::fromRoute(*merged, &geometry, 83);
        require(layout.channels[0].ambiguous && layout.channels[0].placement == Meter::Placement::unplaced,
            "ambiguous many-to-one binding cannot select the first contributing speaker");

        Plan::TrustedRoles trusted {std::vector<Role>(device.outputNames.size(), Role::unknown)};
        for (int c = 0; c < channels; ++c) (*trusted)[static_cast<std::size_t>(c * 2 + 1)] = roles[static_cast<std::size_t>(c)];
        const auto trustedMerge = Plan::validate(draft, device, bus, trusted).approved(true);
        layout = Builder::fromRoute(*trustedMerge, &geometry, 84);
        require(layout.channels[0].placement == Meter::Placement::directional && layout.channels[0].azimuthDegrees == -30
            && layout.channels[0].ambiguous, "actual physical role anchors a merge without hiding convergence");
        draft.outputsBySpeaker[0] = {3}; draft.outputsBySpeaker[1] = {1};
        const auto crossed = Plan::validate(draft, device, bus, trusted).approved(true);
        layout = Builder::fromRoute(*crossed, &geometry, 85);
        require(layout.channels[0].azimuthDegrees == -30 && layout.channels[1].azimuthDegrees == 30,
            "crossed source routes do not exchange the physical speaker directions");
        draft.outputsBySpeaker[1].clear();
        layout = Builder::fromRoute(*Plan::validate(draft, device, bus).approved(true), &geometry, 86);
        require(layout.unmappedSpeakerCount == 1, "unmapped canonical speaker diagnostic retained");
    }
    near(Builder::positionWeight(60, 0), 1.41, 1e-12, "custom left sector boundary");
    near(Builder::positionWeight(-120, 29.9f), 1.41, 1e-12, "custom right sector boundary");
    near(Builder::positionWeight(90, 30), 1.0, 1e-12, "custom height boundary");
    near(Builder::positionWeight(120.1f, 0), 1.0, 1e-12, "custom rear boundary");
    std::cout << "PASS: Stereo/5.1/7.1/7.1.4/custom geometry, active binding, physical semantics and stale diagnostics\n";
}
}

int main(int argc,char** argv)
{
    try
    {
        if(argc==3 && juce::String(argv[1])=="--resource-probe")
        { programmeResourceProbe(juce::File(juce::String::fromUTF8(argv[2]))); return 0; }
        require(argc==1,"use no arguments, or --resource-probe with an absolute receipt path");
        calibrationAndGates(); windowsAndTransport(); referenceNoiseAndImpulse();
        physicalRoute(); multichannel(); backlogAndConcurrency(); geometryBindings();
        std::cout << "Spatial Energy measurement tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
