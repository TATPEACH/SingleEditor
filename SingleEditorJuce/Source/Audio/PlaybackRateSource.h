#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <vector>

struct SRC_STATE_tag;

namespace singleeditor
{
// Consumer-owned rate conversion. Positions exposed to the transport are in
// device frames; the borrowed source always receives source frames/rate.
// Configure/prepare/release only while callbacks are quiescent. Seek and render
// share one lock and one multichannel converter, including after starvation.
class PlaybackRateSource final : public juce::PositionableAudioSource
{
public:
    enum class Mode { unprepared, bypassed, converting, unavailable };
    enum class Error { none, configuration, allocation, blockShape, converter };
    struct Snapshot
    {
        Mode mode = Mode::unprepared;
        Error error = Error::none;
        double sourceRate = 0.0, deviceRate = 0.0;
        int channels = 0;
        // Actual source frames fetched ahead of the audible timeline, expressed
        // in device frames. This is lookahead, not added playback time/delay.
        int bufferedLookaheadFrames = 0;
        static constexpr const char* algorithm = "libsamplerate-0.2.2-sinc-medium";
    };

    PlaybackRateSource(juce::PositionableAudioSource&, double sourceRate, int channelCapacity);
    ~PlaybackRateSource() override;
    Snapshot snapshot() const noexcept;
    void setRenderChannelCount(int); // quiescent preparation only
    void prepareToPlay(int expectedBlockSize, double deviceRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo&) override;
    void setNextReadPosition(juce::int64) override;
    juce::int64 getNextReadPosition() const override;
    juce::int64 getTotalLength() const override;
    bool isLooping() const override { return false; }

private:
    void resetAt(juce::int64 deviceFrame);
    void fail(Error) noexcept;
    juce::PositionableAudioSource& input;
    const double sourceRate;
    int channels;
    juce::CriticalSection lock;
    SRC_STATE_tag* converter = nullptr;
    juce::AudioBuffer<float> planar;
    std::vector<juce::AudioBuffer<float>> channelViews;
    std::vector<float> interleavedInput, interleavedOutput;
    int blockCapacity = 0, inputFrames = 0, inputOffset = 0, renderChannels = 0;
    bool sourcePrepared = false, drained = false;
    static constexpr int inputBlockFrames = 256;
    std::atomic<Mode> mode { Mode::unprepared };
    std::atomic<Error> error { Error::none };
    std::atomic<double> deviceRate { 0.0 };
    std::atomic<juce::int64> outputPosition { 0 };
    std::atomic<int> lookahead { 0 };
    std::atomic<int> publishedChannels { 0 };
};
}
