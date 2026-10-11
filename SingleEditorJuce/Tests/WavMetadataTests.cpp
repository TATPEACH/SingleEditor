#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

// Hand-authored input exercises the reader before any writer can initialize
// its metadata table. Four workers enter that first lookup concurrently.
juce::MemoryBlock inputWave()
{
    const juce::String xml = "<BWFXML><IXML_VERSION>3.01</IXML_VERSION><ASWG>"
        "<contentType>dialogue &amp; effects</contentType><billingCode>LAST</billingCode>"
        "<notAnAswgKey>ignored</notAnAswgKey><ContentType>wrong case</ContentType>"
        "</ASWG></BWFXML>";
    const auto xmlBytes = static_cast<int>(xml.getNumBytesAsUTF8());
    juce::MemoryOutputStream out;
    out.write("RIFF", 4);
    out.writeInt(36 + 8 + xmlBytes + (xmlBytes & 1) + 8);
    out.write("WAVEfmt ", 8);
    out.writeInt(16);
    out.writeShort(1); out.writeShort(1);
    out.writeInt(48000); out.writeInt(96000);
    out.writeShort(2); out.writeShort(16);
    out.write("iXML", 4); out.writeInt(xmlBytes);
    out.write(xml.toRawUTF8(), static_cast<size_t>(xmlBytes));
    if ((xmlBytes & 1) != 0) out.writeByte(0);
    out.write("data", 4); out.writeInt(8);
    for (const short sample : { 0, 4096, -8192, 16384 }) out.writeShort(sample);
    return out.getMemoryBlock();
}

void readFirstUse(const juce::MemoryBlock& block)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(
        new juce::MemoryInputStream(block, false), true));
    require(reader != nullptr && reader->lengthInSamples == 4 && reader->numChannels == 1,
        "hand-authored WAV admission");
    require(reader->metadataValues[juce::WavAudioFormat::aswgContentType] == "dialogue & effects",
        "ASWG first key and XML entity parsing");
    require(reader->metadataValues[juce::WavAudioFormat::aswgBillingCode] == "LAST",
        "ASWG last key remains present");
    require(!reader->metadataValues.getAllKeys().contains("notAnAswgKey")
        && !reader->metadataValues.getAllKeys().contains("ContentType"), "unknown keys and case variants stay excluded");
    juce::AudioBuffer<float> pcm(1, 4);
    require(reader->read(&pcm, 0, 4, 0, true, false), "read original PCM");
    require(pcm.getSample(0, 1) == 0.125f && pcm.getSample(0, 2) == -0.25f,
        "metadata lookup does not change PCM");
}

void roundTrip(bool includeMetadata)
{
    const auto value = juce::String::fromUTF8("voice & <mix> \xe4\xb8\xad\xe6\x96\x87");
    std::unordered_map<juce::String, juce::String> metadata;
    if (includeMetadata)
    {
        metadata[juce::WavAudioFormat::aswgContentType] = value;
        metadata[juce::WavAudioFormat::aswgActorName] = value;
        metadata[juce::WavAudioFormat::aswgBillingCode] = value;
        metadata["notAnAswgKey"] = "excluded";
    }
    juce::MemoryBlock block;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream>(block, false);
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(1)
        .withBitsPerSample(16).withMetadataValues(metadata);
    auto writer = format.createWriterFor(stream, options);
    require(writer != nullptr, "WAV writer admission");
    juce::AudioBuffer<float> source(1, 4);
    for (int i = 0; i < 4; ++i) source.setSample(0, i, static_cast<float>(i) * 0.125f);
    require(writer->writeFromAudioSampleBuffer(source, 0, 4), "write WAV PCM");
    writer.reset();
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(
        new juce::MemoryInputStream(block, false), true));
    require(reader != nullptr && reader->lengthInSamples == 4, "reopen WAV output");
    for (const auto* key : { juce::WavAudioFormat::aswgContentType, juce::WavAudioFormat::aswgActorName,
                            juce::WavAudioFormat::aswgBillingCode })
        require(includeMetadata ? reader->metadataValues[key] == value : !reader->metadataValues.containsKey(key),
            "ASWG UTF8/entity value round trip or empty metadata");
    require(!reader->metadataValues.containsKey("notAnAswgKey"), "writer excludes unknown ASWG keys");
    juce::AudioBuffer<float> pcm(1, 4);
    require(reader->read(&pcm, 0, 4, 0, true, false), "read roundtrip PCM");
    for (int i = 0; i < 4; ++i)
        require(std::abs(pcm.getSample(0, i) - source.getSample(0, i)) < 0.00004f, "PCM roundtrip tolerance");
}
}

int main()
{
    try
    {
        const auto input = inputWave();
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::array<std::exception_ptr, 4> failures;
        std::vector<std::thread> workers;
        for (size_t i = 0; i < failures.size(); ++i)
            workers.emplace_back([&, i]
            {
                ++ready;
                while (!go.load()) std::this_thread::yield();
                try { readFirstUse(input); } catch (...) { failures[i] = std::current_exception(); }
            });
        while (ready.load() != static_cast<int>(failures.size())) std::this_thread::yield();
        go = true;
        for (auto& worker : workers) worker.join();
        for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
        roundTrip(true);
        roundTrip(false);
        std::cout << "WAV metadata: concurrent first reader, known/unknown/case keys, UTF8/XML roundtrip, ordinary WAV and PCM passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
