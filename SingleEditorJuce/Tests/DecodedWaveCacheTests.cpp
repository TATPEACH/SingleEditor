#include "Audio/DecodedWaveCache.h"
#include <iostream>

using Cache = singleeditor::media::DecodedWaveCache;
namespace
{
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void writeWave(const juce::File& file, int sampleValue = 1, int channels = 2)
{
    juce::MemoryOutputStream out;
    out.write("RIFF", 4); out.writeInt(36 + 64); out.write("WAVEfmt ", 8);
    out.writeInt(16); out.writeShort(1); out.writeShort(static_cast<short>(channels));
    out.writeInt(48000); out.writeInt(48000 * channels * 2);
    out.writeShort(static_cast<short>(channels * 2)); out.writeShort(16);
    out.write("data", 4); out.writeInt(64);
    for (int i = 0; i < 32; ++i) out.writeShort(static_cast<short>(sampleValue + i));
    require(file.replaceWithData(out.getData(), out.getDataSize()), "write owned wave");
}
}
int main()
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = temp.getChildFile("SingleEditor-WaveCacheTest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private fixture");
    const auto source = root.getChildFile("source.media"), producer = root.getChildFile("decoder.bin");
    const auto wave = root.getChildFile("located-by-hash.wav");
    require(source.replaceWithText("source version one") && producer.replaceWithText("producer version one"), "seed identities");
    auto context = Cache::capture(source, producer, "test-pcm16/job-v1");
    require(context.has_value(), "capture explicit optional request");
    Cache::LookupReason reason;
    writeWave(wave);
    require(!Cache::reusable(*context, wave, {}, &reason) && reason == Cache::LookupReason::sealUnavailable,
            "unsealed WAV reports the actual unavailable seal");
    require(Cache::inspectComplete(wave).has_value() && Cache::seal(*context, wave), "validate and seal complete output");
    require(Cache::reusable(*context, wave, {}, &reason) && reason == Cache::LookupReason::reused,
            "exact source/producer/policy and body reuse reports a hit");
    const auto unchangedSeal = juce::SHA256(Cache::sealFile(wave));
    require(!Cache::reusable(*context, wave, [] { return false; }, &reason)
            && reason == Cache::LookupReason::workEnded && juce::SHA256(Cache::sealFile(wave)) == unchangedSeal,
            "interrupted lookup reports stopped work and leaves the completed cache untouched");
    auto otherPolicy = Cache::capture(source, producer, "test-pcm16/job-v2");
    require(!Cache::reusable(*otherPolicy, wave, {}, &reason) && reason == Cache::LookupReason::policyChanged,
            "new resource policy reports why old output is not reused");
    const auto otherSource = root.getChildFile("other-source.media");
    require(otherSource.replaceWithText("source version one"), "seed same-content different locator");
    auto collision = Cache::capture(otherSource, producer, context->policy);
    require(!Cache::reusable(*collision, wave, {}, &reason) && reason == Cache::LookupReason::sourceChanged,
            "different source identity is the reported cache miss");
    writeWave(wave, 9);
    require(Cache::inspectComplete(wave).has_value() && !Cache::reusable(*context, wave),
            "same-size readable body mutation is rejected by hash");
    require(Cache::seal(*context, wave), "seal replacement generation");
    juce::MemoryBlock originalSeal;
    require(Cache::sealFile(wave).loadFileAsData(originalSeal) && originalSeal.getSize() == 184, "fixed-size manifest");
    require(Cache::sealFile(wave).replaceWithData(originalSeal.getData(), 80), "simulate interrupted seal write");
    require(!Cache::reusable(*context, wave), "partial seal is a cache miss");
    require(Cache::sealFile(wave).replaceWithData(originalSeal.getData(), originalSeal.getSize()), "restore owned seal");
    static_cast<unsigned char*>(originalSeal.getData())[8] = 2;
    require(Cache::sealFile(wave).replaceWithData(originalSeal.getData(), originalSeal.getSize()), "future schema fixture");
    require(!Cache::reusable(*context, wave), "unsupported schema is inert");
    require(Cache::seal(*context, wave), "restore current seal");
    require(source.replaceWithText("source version two changed size"), "change source during decode");
    require(!Cache::seal(*context, wave) && !Cache::reusable(*context, wave, {}, &reason)
            && reason == Cache::LookupReason::contextChanged, "stale context cannot publish or reuse and reports the actual change");
    context = Cache::capture(source, producer, context->policy);
    require(Cache::seal(*context, wave), "new source context publishes");
    require(producer.replaceWithText("updated producer different bytes"), "update decoder");
    require(!Cache::reusable(*context, wave), "producer change invalidates cache");
    context = Cache::capture(source, producer, context->policy);
    writeWave(wave, 1, 33);
    require(!Cache::inspectComplete(wave) && !Cache::seal(*context, wave), "hostile channel dimensions rejected before reader creation");
    writeWave(wave);
    juce::MemoryBlock truncated;
    require(wave.loadFileAsData(truncated), "read owned fixture");
    require(wave.replaceWithData(truncated.getData(), truncated.getSize() - 2), "truncate declared body");
    require(!Cache::inspectComplete(wave) && !Cache::seal(*context, wave), "truncated complete marker cannot admit a partial body");
    require(root.isAChildOf(temp) && root.getParentDirectory() == temp
            && root.getFileName().startsWith("SingleEditor-WaveCacheTest-"), "owned fixture boundary");
    require(root.deleteRecursively(), "remove private fixture");
    std::cout << "DecodedWaveCacheTests passed (schema, source/producer/policy, body integrity, incomplete publication, dimensions)\n";
}
