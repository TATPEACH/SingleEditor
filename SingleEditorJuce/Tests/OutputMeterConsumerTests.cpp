#include "Audio/AudioEngine.h"
#include "UI/Components/MeterBridgeComponent.h"
#include "UI/Components/OutputMeterPanelComponent.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace singleeditor
{
int runOutputMeterConsumerTests(const juce::File& artifacts)
{
    if (artifacts.exists() || artifacts.createDirectory().failed())
        throw std::runtime_error("Use a new output-meter artifact directory");
    juce::Array<juce::var> results;
    int failures = 0;
    const auto check = [&](bool ok, const char* name)
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("name", name); row->setProperty("status", ok ? "pass" : "failed");
        results.add(juce::var(row));
        if (!ok) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
    };
    juce::AudioBuffer<float> source(2, 8192);
    for (int sample = 0; sample < source.getNumSamples(); ++sample)
    {
        source.setSample(0, sample, 0.25f * std::sin(static_cast<float>(sample) * 0.037f));
        source.setSample(1, sample, 0.10f * std::sin(static_cast<float>(sample) * 0.051f));
    }
    const auto file = artifacts.getChildFile("stereo.wav");
    {
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        juce::WavAudioFormat format;
        auto options = juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24);
        auto writer = format.createWriterFor(stream, options);
        if (!writer || !writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples()))
            throw std::runtime_error("Cannot write meter source");
    }
    SingleTrackDocument document;
    AudioEngine engine(document);
    if (!engine.loadFile(file)) throw std::runtime_error("Cannot load meter source");
    engine.setMonitorGainLinear(1.0f);
    juce::AudioBuffer<float> rendered(2, 1024);
    check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0
              && engine.renderTransportBlockForDiagnostics(0, rendered)
              && engine.getOutputMeterSnapshot().channelCount == 0 && rendered.getMagnitude(0, 1024) > 0.2f,
          "ordinary actual transport renders audio without an output-meter producer");
    {
        auto first = engine.acquireOutputMeterAnalysisLease();
        auto second = engine.acquireOutputMeterAnalysisLease();
        check(first && second && engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 2,
              "two explicit consumers share the producer");
        check(engine.renderTransportBlockForDiagnostics(0, rendered), "leased actual transport renders");
        OutputMeterAnalyzer reference;
        std::array<SpeakerRole, OutputMeterSnapshot::maxChannels> roles {};
        roles[0] = SpeakerRole::L; roles[1] = SpeakerRole::R;
        reference.prepare(48000, 2, roles);
        const auto expected = reference.process(rendered, 0, rendered.getNumSamples());
        const auto actual = engine.getOutputMeterSnapshot();
        bool equal = actual.channelCount == 2 && std::abs(actual.momentaryLufs - expected.momentaryLufs) < 1.0e-5f;
        for (int channel = 0; channel < 2; ++channel)
            equal = equal && actual.roles[channel] == expected.roles[channel]
                && std::abs(actual.truePeakLinear[channel] - expected.truePeakLinear[channel]) < 1.0e-6f
                && std::abs(actual.rmsLinear[channel] - expected.rmsLinear[channel]) < 1.0e-6f
                && std::abs(actual.kWeightedContributionDb[channel] - expected.kWeightedContributionDb[channel]) < 1.0e-5f;
        check(equal, "leased post-route TP RMS and LUFS match the existing analyzer on actual rendered samples");
        first = {};
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 1
                  && engine.renderTransportBlockForDiagnostics(256, rendered)
                  && engine.getOutputMeterSnapshot().channelCount == 2,
              "older consumer release preserves the remaining consumer");
        auto moved = std::move(second);
        check(!second && moved && engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 1,
              "lease move transfers exactly one consumer");
    }
    check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0
              && engine.renderTransportBlockForDiagnostics(512, rendered)
              && engine.getOutputMeterSnapshot().channelCount == 0,
          "last release retires the producer and clears stale levels while playback continues");
    {
        juce::Component host;
        host.setBounds(80, 80, 640, 420);
        host.addToDesktop(0);
        host.setVisible(true);
        OutputMeterPanelComponent panel(engine);
        MeterBridgeComponent bridge(engine);
        panel.setBounds(0, 0, 480, 360); bridge.setBounds(0, 365, 200, 36);
        host.addAndMakeVisible(panel); host.addAndMakeVisible(bridge);
        check(panel.isShowing() && bridge.isShowing() && engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "visible native views remain dormant before CoreReady");
        panel.setCoreReady(true); bridge.setCoreReady(true);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 2,
              "post-Core native panel and toolbar share two visible leases");
        host.setVisible(false);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "hiding an ancestor immediately releases both view leases");
        host.setVisible(true);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 2,
              "showing an ancestor reacquires the real consumers");
        panel.setVisible(false);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 1,
              "hidden output Mag releases independently of the toolbar");
        bridge.setCoreReady(false);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "new document readiness withdraws the remaining producer");
        panel.setVisible(true); panel.setCoreReady(false);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "visible old view cannot retain analysis across readiness withdrawal");
        panel.setCoreReady(true);
        host.removeChildComponent(&panel);
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "removing a still-visible view releases its lease");
        host.setVisible(false); host.removeFromDesktop();
    }
    {
        std::atomic<int> blocks { 0 }, errors { 0 };
        std::jthread callback([&](std::stop_token stop)
        {
            // This one-shot diagnostic renderer prepares/starts/stops each
            // call. Reach the real fixture EOF so JUCE acknowledges stop in
            // the rendered block; a short block otherwise waits for a later
            // device callback which this device-free fixture never supplies.
            juce::AudioBuffer<float> block(2, source.getNumSamples());
            while (!stop.stop_requested())
            {
                if (!engine.renderTransportBlockForDiagnostics(0, block)) ++errors;
                ++blocks;
            }
        });
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
        while (blocks.load() < 4 && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::yield();
        for (int n = 0; n < 32; ++n)
        {
            auto lease = engine.acquireOutputMeterAnalysisLease();
            juce::Thread::yield();
        }
        callback.request_stop(); callback.join();
        std::cout << "Concurrent meter: blocks=" << blocks.load()
                  << " render-errors=" << errors.load()
                  << " consumers=" << engine.getOutputMeterAnalysisLeaseCountForDiagnostics()
                  << " channels=" << engine.getOutputMeterSnapshot().channelCount << '\n';
        check(blocks.load() >= 4 && errors.load() == 0
                  && engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0
                  && engine.getOutputMeterSnapshot().channelCount == 0,
              "control-thread admission and retirement coexist with actual render callbacks");
    }
    AudioEngine::OutputMeterAnalysisLease lateLease;
    {
        SingleTrackDocument otherDocument;
        auto otherEngine = std::make_unique<AudioEngine>(otherDocument);
        lateLease = otherEngine->acquireOutputMeterAnalysisLease();
        otherEngine.reset();
    }
    lateLease = {};
    check(true, "a task lease can be released after its engine is destroyed");
    auto* report = new juce::DynamicObject();
    report->setProperty("status", failures == 0 ? "pass" : "failed");
    report->setProperty("checks", results.size()); report->setProperty("failures", failures);
    report->setProperty("results", results);
    artifacts.getChildFile("receipt.json").replaceWithText(juce::JSON::toString(juce::var(report), true));
    std::cout << "Output Meter consumer checks: " << results.size() << ", failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
}
