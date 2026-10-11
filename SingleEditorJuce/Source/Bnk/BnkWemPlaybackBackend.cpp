#include "Bnk/BnkWemPlaybackBackend.h"
#include "Bnk/BnkMediaLocator.h"
#include "Bnk/BnkPreviewRequest.h"
#include "Bnk/BnkStateSelection.h"
#include "Bnk/BoundedSidecarInput.h"
#include "Audio/ReaderAdmission.h"
#include "Audio/SourceFileLease.h"
#include "Audio/VgmstreamDecodeService.h"
#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <array>
#include <optional>
#include <unordered_map>

namespace singleeditor
{
namespace
{
struct BackendWork final
{
    media::OptionalMediaOperation::Ptr operation;
    bool resolvedInputs = false;
};
using Policy = media::OptionalMediaFailureState;
bool proceed(const BackendWork& work, Policy::Stage stage = Policy::Stage::decode)
{ return !work.operation || (work.operation->module() == Policy::Module::bank && work.operation->continueWork(stage)); }
void fail(const BackendWork& work, Policy::Reason reason, Policy::Stage stage = Policy::Stage::decode)
{ if (work.operation) work.operation->fail(reason, stage); }

struct PlaybackMediaTarget
{
    uint32_t mediaId = 0;
    juce::String displayName;
    juce::String shortName;
    juce::String relativePath;
    juce::File mediaFile;
    bool allowBankExtract = false;
    juce::File sourceFile;
};

std::unique_ptr<juce::AudioFormatReader> createReaderForMedia(const juce::File& file, const BackendWork& work)
{
    if (!proceed(work) || !file.existsAsFile())
        return {};

    if (work.operation)
    {
        // This worker's direct format readers cannot implicitly launch another
        // optional decoder outside the admitted bank work.
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (auto stream = file.createInputStream())
            if (auto* reader = formats.createReaderFor(std::move(stream)))
                return std::unique_ptr<juce::AudioFormatReader>(reader);
        if (!proceed(work)) return {};
        if (file.hasFileExtension("wem"))
            return VgmstreamDecodeService::createReaderForDecodedFile(file, nullptr, work.operation);
        fail(work, Policy::Reason::cacheInvalid);
        return {};
    }
    return SingleTrackDocument::createReaderForFile(file);
}

juce::File getEmbeddedMediaCacheDir(const BnkDocument::State& state)
{
    const auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("SingleEditor")
                          .getChildFile("bnk_media_cache");
    const auto cacheIdentity = state.info.file.getFullPathName()
                             + "|"
                             + juce::String(state.info.file.getSize())
                             + "|"
                             + juce::String(state.info.file.getLastModificationTime().toMilliseconds());
    const auto bankKey = juce::String::toHexString(static_cast<juce::int64>(cacheIdentity.hashCode64()));
    return root.getChildFile(bankKey);
}

juce::File extractEmbeddedMediaFile(const BnkDocument::State& state,
                                    uint32_t mediaId,
                                    juce::StringArray& diagnostics, const BackendWork& work)
{
    const auto checkpoint = [&] { return proceed(work, Policy::Stage::cache); };
    if (!checkpoint()) return {};
    if (!state.info.file.existsAsFile() || state.dataChunkStart <= 0 || state.dataChunkSize <= 0 || mediaId == 0)
        return {};

    const auto entry = std::find_if(state.mediaEntries.begin(),
                                    state.mediaEntries.end(),
                                    [mediaId](const BnkDocument::MediaEntry& current)
                                    {
                                        return current.id == mediaId;
                                    });
    if (entry == state.mediaEntries.end() || entry->size == 0)
        return {};

    const auto entryEnd = static_cast<int64_t>(entry->offset) + static_cast<int64_t>(entry->size);
    if (entryEnd > state.dataChunkSize)
    {
        diagnostics.add("Embedded media " + juce::String(mediaId) + " points outside the DATA chunk.");
        return {};
    }

    constexpr uint32_t maximumEmbeddedBytes = 512 * 1024 * 1024;
    if (entry->size > maximumEmbeddedBytes || !state.isSourceCurrent())
    {
        diagnostics.add("Embedded media exceeds the extraction policy or its bank changed; reopen the bank.");
        return {};
    }
    const auto cacheDir = getEmbeddedMediaCacheDir(state);
    if (cacheDir.createDirectory().failed()) return {};
    const auto cachedFile = cacheDir.getChildFile(juce::String(mediaId) + ".wem");
    juce::FileInputStream input(state.info.file);
    const auto start = state.dataChunkStart + static_cast<int64_t>(entry->offset);
    if (!input.openedOk() || start < 0 || start + entry->size > input.getTotalLength()) return {};
    juce::SubregionStream region(&input, start, entry->size, false);
    const auto expectedHash = media::checkedStreamHash(region, entry->size, checkpoint);
    if (!expectedHash || region.getPosition() != entry->size || !state.isSourceCurrent()) return {};
    if (cachedFile.existsAsFile() && cachedFile.getSize() == static_cast<int64_t>(entry->size))
    {
        juce::FileInputStream cached(cachedFile);
        if (cached.openedOk() && media::checkedStreamHash(cached, entry->size, checkpoint) == expectedHash
            && cached.getPosition() == entry->size && state.isSourceCurrent())
            return cachedFile;
    }

    const auto staging = cachedFile.getSiblingFile(cachedFile.getFileName() + "." + juce::Uuid().toString() + ".tmp");
    bool complete = false;
    {
        juce::FileOutputStream output(staging);
        if (output.openedOk() && input.setPosition(start))
        {
            std::array<char, 64 * 1024> buffer {};
            int64_t remaining = entry->size;
            while (remaining > 0)
            {
                if (!checkpoint()) break;
                const auto count = static_cast<int>(juce::jmin<int64_t>(remaining, buffer.size()));
                if (input.read(buffer.data(), count) != count || !output.write(buffer.data(), static_cast<size_t>(count)))
                    break;
                remaining -= count;
            }
            output.flush();
            complete = remaining == 0 && output.getStatus().wasOk()
                && output.getPosition() == entry->size && state.isSourceCurrent();
        }
    }
    if (complete)
    {
        juce::FileInputStream readback(staging);
        complete = readback.openedOk() && readback.getTotalLength() == entry->size
            && media::checkedStreamHash(readback, entry->size, checkpoint) == expectedHash && readback.getPosition() == entry->size;
    }
    if (complete && checkpoint() && state.isSourceCurrent() && staging.replaceFileIn(cachedFile))
    {
        diagnostics.add("Extracted and validated embedded media " + juce::String(mediaId) + " from the BNK DATA chunk.");
        return cachedFile;
    }
    staging.deleteFile();
    diagnostics.add("Failed to commit embedded media " + juce::String(mediaId) + " from the BNK DATA chunk.");
    return {};
}

juce::File tryFindOriginalMedia(const BnkDocument::State& state, const PlaybackMediaTarget& media, const BackendWork& work)
{
    BoundedSidecarInput inputBudget(nullptr, [&] { return proceed(work, Policy::Stage::admission); });
    const auto source=bnkmedia::findOriginal(media.sourceFile,state.sourceFilesRoot,state.projectRoot,
        media.relativePath,media.shortName);
    if (inputBudget.wasLimited()) { fail(work, Policy::Reason::parserBounds, Policy::Stage::admission); return {}; }
    return source;
}

std::unique_ptr<juce::AudioFormatReader> createFallbackReader(const BnkDocument::State& state,
                                                              const PlaybackMediaTarget& media,
                                                              BnkMediaResolveMode resolveMode,
                                                              juce::StringArray& diagnostics, const BackendWork& work)
{
    if (!proceed(work)) return {};
    if (work.resolvedInputs)
    {
        // The request already selected an exact external input or bank span.
        // Missing/replaced input is a failure, never permission to resolve again.
        if (media.mediaFile != juce::File()) return createReaderForMedia(media.mediaFile, work);
        if (media.allowBankExtract)
            return createReaderForMedia(extractEmbeddedMediaFile(state, media.mediaId, diagnostics, work), work);
        return {};
    }
    const auto preferSource = resolveMode == BnkMediaResolveMode::sourceMedia;
    if (preferSource)
    {
        const auto originalFile = tryFindOriginalMedia(state, media, work);
        if (!proceed(work)) return {};
        if (originalFile.existsAsFile())
        {
            diagnostics.add("Using source media " + originalFile.getFileName() + " from Originals.");
            return createReaderForMedia(originalFile, work);
        }
    }

    // Resolve an available location before decoding. An actual failed decode
    // must not silently switch to a different producer/source or retry itself.
    if (media.mediaFile.existsAsFile()) return createReaderForMedia(media.mediaFile, work);

    if (media.mediaId != 0)
    {
        const auto generatedWem = state.info.file.getParentDirectory().getChildFile(juce::String(media.mediaId) + ".wem");
        if (generatedWem.existsAsFile())
        {
            diagnostics.add("Resolved streamed media " + juce::String(media.mediaId) + " from the bank folder.");
            return createReaderForMedia(generatedWem, work);
        }
    }

    const auto embedded = std::find_if(state.mediaEntries.begin(), state.mediaEntries.end(),
        [&](const auto& entry) { return entry.id == media.mediaId; });
    if (media.allowBankExtract && embedded != state.mediaEntries.end())
    {
        const auto embeddedFile = extractEmbeddedMediaFile(state, media.mediaId, diagnostics, work);
        return createReaderForMedia(embeddedFile, work);
    }

    if (!preferSource)
    {
        const auto originalFile = tryFindOriginalMedia(state, media, work);
        if (!proceed(work)) return {};
        if (originalFile.existsAsFile())
        {
            diagnostics.add("Resolved source media " + originalFile.getFileName() + ".");
            return createReaderForMedia(originalFile, work);
        }
    }

    diagnostics.add("Unable to create a reader for media " + media.shortName + ".");
    return {};
}

struct DecodedMedia
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
    int bitsPerSample = 16;
};

constexpr uint64_t maximumPreviewBytes = 256ULL * 1024 * 1024;
bool allocatePreview(juce::AudioBuffer<float>& buffer, int channels, int64_t frames)
{
    const auto admitted = media::admitMaterialisation(static_cast<uint64_t>(channels), frames);
    if (!admitted || admitted.floatBytes > maximumPreviewBytes) return false;
    buffer.setSize(channels, static_cast<int>(frames));
    buffer.clear();
    return true;
}
std::optional<DecodedMedia> readDecoded(juce::AudioFormatReader* reader, juce::StringArray& diagnostics, const BackendWork& work)
{
    if (!proceed(work)) return std::nullopt;
    if (reader == nullptr || !media::admitReader(reader->sampleRate, reader->numChannels, reader->lengthInSamples))
    {
        fail(work, Policy::Reason::cacheInvalid);
        return std::nullopt;
    }
    DecodedMedia decoded;
    if (!allocatePreview(decoded.buffer, static_cast<int>(reader->numChannels), reader->lengthInSamples))
    {
        diagnostics.add("This media exceeds the 256 MiB BNK preview buffer policy; open the media directly for streaming.");
        fail(work, Policy::Reason::parserBounds);
        return std::nullopt;
    }
    decoded.sampleRate = reader->sampleRate; decoded.bitsPerSample = reader->bitsPerSample;
    for (int offset = 0; offset < decoded.buffer.getNumSamples(); offset += 4096)
    {
        if (!proceed(work)) return std::nullopt;
        const auto count = juce::jmin(4096, decoded.buffer.getNumSamples() - offset);
        if (!reader->read(&decoded.buffer, offset, count, offset, true, reader->numChannels > 1))
        {
            diagnostics.add("BNK media decoding failed before a complete preview was available.");
            fail(work, Policy::Reason::cacheInvalid);
            return std::nullopt;
        }
    }
    return decoded;
}

std::optional<DecodedMedia> decodeMedia(const BnkDocument::State& state,
                                        const BnkDocument::EventDescriptor::MediaReference& media,
                                        BnkMediaResolveMode resolveMode,
                                        juce::StringArray& diagnostics, const BackendWork& work)
{
    PlaybackMediaTarget target;
    target.mediaId = media.mediaId;
    target.displayName = media.displayName;
    target.shortName = media.shortName;
    target.relativePath = media.relativePath;
    target.mediaFile = media.mediaFile;
    target.sourceFile = media.sourceFile;
    target.allowBankExtract = true;

    auto reader = createFallbackReader(state, target, resolveMode, diagnostics, work);
    return readDecoded(reader.get(), diagnostics, work);
}

std::optional<DecodedMedia> decodeMedia(const BnkDocument::State& state,
                                        const BnkDocument::PlayableMediaDescriptor& media,
                                        BnkMediaResolveMode resolveMode,
                                        juce::StringArray& diagnostics, const BackendWork& work)
{
    PlaybackMediaTarget target;
    target.mediaId = media.mediaId;
    target.displayName = media.displayName;
    target.shortName = media.shortName;
    target.relativePath = media.relativePath;
    target.mediaFile = media.mediaFile;
    target.sourceFile = media.sourceFile;
    target.allowBankExtract = media.extractableFromBank;

    auto reader = createFallbackReader(state, target, resolveMode, diagnostics, work);
    return readDecoded(reader.get(), diagnostics, work);
}

std::optional<DecodedMedia> renderRandom(const BnkDocument::State& state,
                                         const BnkDocument::EventDescriptor& event,
                                         BnkMediaResolveMode resolveMode,
                                         juce::StringArray& diagnostics, const BackendWork& work)
{
    std::vector<const BnkDocument::EventDescriptor::MediaReference*> candidates;
    for (const auto& media : event.media)
    {
        if (media.mediaFile.existsAsFile() || media.sourceFile.existsAsFile() || !media.shortName.isEmpty())
            candidates.push_back(&media);
    }

    if (candidates.empty())
        return std::nullopt;

    const auto index = juce::Random::getSystemRandom().nextInt(static_cast<int>(candidates.size()));
    diagnostics.add("Resolved random event to " + candidates[static_cast<std::size_t>(index)]->displayName + ".");
    return decodeMedia(state, *candidates[static_cast<std::size_t>(index)], resolveMode, diagnostics, work);
}

std::optional<DecodedMedia> renderSwitchDefault(const BnkDocument::State& state,
                                                const BnkDocument::EventDescriptor& event,
                                                const BnkPlaybackContext& context,
                                                BnkMediaResolveMode resolveMode,
                                                juce::StringArray& diagnostics, const BackendWork& work)
{
    const BnkDocument::EventDescriptor::MediaReference* fallback = nullptr;
    const BnkDocument::EventDescriptor::MediaReference* defaultMedia = nullptr;
    const BnkDocument::EventDescriptor::MediaReference* selectedMedia = nullptr;
    for (const auto& media : event.media)
    {
        if (fallback == nullptr && (media.mediaFile.existsAsFile() || media.sourceFile.existsAsFile() || !media.shortName.isEmpty()))
            fallback = &media;

        if (selectedMedia == nullptr && context.switchValueToken.isNotEmpty() && media.switchValueToken == context.switchValueToken)
            selectedMedia = &media;

        if (defaultMedia == nullptr && media.isDefaultSwitch)
            defaultMedia = &media;
    }

    if (selectedMedia != nullptr)
    {
        diagnostics.add("Resolved switch event to " + selectedMedia->displayName + ".");
        return decodeMedia(state, *selectedMedia, resolveMode, diagnostics, work);
    }

    if (defaultMedia != nullptr)
    {
        diagnostics.add("Resolved switch event to default switch media " + defaultMedia->displayName + ".");
        return decodeMedia(state, *defaultMedia, resolveMode, diagnostics, work);
    }

    if (fallback != nullptr)
    {
        diagnostics.add("Default switch media was not identified, using the first available switch candidate.");
        return decodeMedia(state, *fallback, resolveMode, diagnostics, work);
    }

    return std::nullopt;
}

std::optional<DecodedMedia> renderLayered(const BnkDocument::State& state,
                                          const BnkDocument::EventDescriptor& event,
                                          BnkMediaResolveMode resolveMode,
                                          juce::StringArray& diagnostics, const BackendWork& work)
{
    std::vector<DecodedMedia> decodedLayers;
    if (event.media.size() > 8)
    { fail(work, Policy::Reason::parserBounds); diagnostics.add("This event exceeds the 8-layer preview limit; no partial mix was created."); return std::nullopt; }
    decodedLayers.reserve(event.media.size());
    uint64_t retainedBytes = 0;

    for (const auto& media : event.media)
    {
        auto decoded = decodeMedia(state, media, resolveMode, diagnostics, work);
        if (!decoded) { diagnostics.add("A required layer could not be decoded; no partial mix was created."); return std::nullopt; }
        retainedBytes += static_cast<uint64_t>(decoded->buffer.getNumChannels()) * decoded->buffer.getNumSamples() * sizeof(float);
        if (retainedBytes > 2 * maximumPreviewBytes) { fail(work, Policy::Reason::parserBounds); return std::nullopt; }
        decodedLayers.push_back(std::move(*decoded));
    }

    if (decodedLayers.empty())
        return std::nullopt;

    if (decodedLayers.size() == 1)
        return std::move(decodedLayers.front());

    int outputChannels = 0;
    int outputSamples = 0;
    double outputRate = decodedLayers.front().sampleRate;
    int bitsPerSample = decodedLayers.front().bitsPerSample;

    for (const auto& layer : decodedLayers)
    {
        if (std::abs(layer.sampleRate - outputRate) > 1.0)
        {
            diagnostics.add("Layer sample rates differ; no partial mix was created.");
            fail(work, Policy::Reason::parserBounds);
            return std::nullopt;
        }
        outputChannels = juce::jmax(outputChannels, layer.buffer.getNumChannels());
        outputSamples = juce::jmax(outputSamples, layer.buffer.getNumSamples());
    }

    if (outputChannels <= 0 || outputSamples <= 0)
        return std::nullopt;

    DecodedMedia mixed;
    mixed.sampleRate = outputRate;
    mixed.bitsPerSample = bitsPerSample;
    if (!proceed(work)) return std::nullopt;
    if (!allocatePreview(mixed.buffer, outputChannels, outputSamples)) { fail(work, Policy::Reason::parserBounds); return std::nullopt; }

    for (const auto& layer : decodedLayers)
    {
        const auto layerChannels = layer.buffer.getNumChannels();
        const auto layerSamples = layer.buffer.getNumSamples();

        for (int ch = 0; ch < outputChannels; ++ch)
        {
            auto* dst = mixed.buffer.getWritePointer(ch);
            const auto sourceChannel = layerChannels == 1 ? 0 : juce::jmin(ch, layerChannels - 1);
            const auto* src = layer.buffer.getReadPointer(sourceChannel);
            for (int offset = 0; offset < layerSamples; offset += 4096)
            {
                if (!proceed(work)) return std::nullopt;
                const auto end = juce::jmin(layerSamples, offset + 4096);
                for (int i = offset; i < end; ++i) dst[i] += src[i];
            }
        }
    }

    const auto normaliseGain = 1.0f / static_cast<float>(juce::jmax(1, static_cast<int>(decodedLayers.size())));
    for (int offset = 0; offset < outputSamples; offset += 4096)
    {
        if (!proceed(work)) return std::nullopt;
        mixed.buffer.applyGain(offset, juce::jmin(4096, outputSamples - offset), normaliseGain);
    }
    diagnostics.add("Mixed " + juce::String(static_cast<int>(decodedLayers.size())) + " layered media streams.");
    return mixed;
}

std::optional<DecodedMedia> renderSequence(const BnkDocument::State& state,
                                           const BnkDocument::EventDescriptor& event,
                                           BnkMediaResolveMode resolveMode,
                                           juce::StringArray& diagnostics, const BackendWork& work)
{
    std::vector<DecodedMedia> decodedSteps;
    if (event.media.size() > 16)
    { fail(work, Policy::Reason::parserBounds); diagnostics.add("This event exceeds the 16-step preview limit; no truncated sequence was created."); return std::nullopt; }
    decodedSteps.reserve(event.media.size());
    uint64_t retainedBytes = 0;

    for (const auto& media : event.media)
    {
        auto decoded = decodeMedia(state, media, resolveMode, diagnostics, work);
        if (!decoded) { diagnostics.add("A required sequence step could not be decoded; no partial preview was created."); return std::nullopt; }
        retainedBytes += static_cast<uint64_t>(decoded->buffer.getNumChannels()) * decoded->buffer.getNumSamples() * sizeof(float);
        if (retainedBytes > 2 * maximumPreviewBytes) { fail(work, Policy::Reason::parserBounds); return std::nullopt; }
        decodedSteps.push_back(std::move(*decoded));
    }

    if (decodedSteps.empty())
        return std::nullopt;

    if (decodedSteps.size() == 1)
        return std::move(decodedSteps.front());

    const auto outputRate = decodedSteps.front().sampleRate;
    const auto bitsPerSample = decodedSteps.front().bitsPerSample;
    int outputChannels = 0;
    int outputSamples = 0;

    for (const auto& step : decodedSteps)
    {
        if (std::abs(step.sampleRate - outputRate) > 1.0)
        {
            diagnostics.add("Sequence sample rates differ; no partial preview was created.");
            fail(work, Policy::Reason::parserBounds);
            return std::nullopt;
        }

        outputChannels = juce::jmax(outputChannels, step.buffer.getNumChannels());
        outputSamples += step.buffer.getNumSamples();
    }

    if (outputChannels <= 0 || outputSamples <= 0)
        return std::nullopt;

    DecodedMedia stitched;
    stitched.sampleRate = outputRate;
    stitched.bitsPerSample = bitsPerSample;
    if (!proceed(work)) return std::nullopt;
    if (!allocatePreview(stitched.buffer, outputChannels, outputSamples)) { fail(work, Policy::Reason::parserBounds); return std::nullopt; }

    auto writeOffset = 0;
    auto appended = 0;
    for (const auto& step : decodedSteps)
    {
        const auto stepChannels = step.buffer.getNumChannels();
        const auto stepSamples = step.buffer.getNumSamples();
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            const auto sourceChannel = stepChannels == 1 ? 0 : juce::jmin(ch, stepChannels - 1);
            for (int offset = 0; offset < stepSamples; offset += 4096)
            {
                if (!proceed(work)) return std::nullopt;
                stitched.buffer.copyFrom(ch, writeOffset + offset, step.buffer, sourceChannel,
                    offset, juce::jmin(4096, stepSamples - offset));
            }
        }

        writeOffset += stepSamples;
        ++appended;
    }

    diagnostics.add("Stitched " + juce::String(appended) + " sequence step(s) into a preview render.");
    return stitched;
}

bool applyStateModifiers(DecodedMedia& decoded,
                         const BnkDocument::EventDescriptor& event,
                         const BnkPlaybackContext& context,
                         const BnkDocument::State& state,
                         juce::String& detail,
                         juce::StringArray& diagnostics, const BackendWork& work)
{
    if (context.stateValueToken.isEmpty())
        return true;

    const auto options = bnkstate::options(state, event);
    const BnkDocument::StateValueDescriptor* selected = nullptr;
    for (const auto& option : options)
    {
        if (!proceed(work)) return false;
        const bool matches = context.stateValueToken == bnkstate::token(option)
            || (option.token.isNotEmpty() && bnkstate::id(context.stateValueToken)==bnkstate::id(option.token));
        if (!matches) continue;
        if (selected) { fail(work, Policy::Reason::parserBounds); return false; }
        selected=&option;
    }
    if (!selected) { fail(work, Policy::Reason::parserBounds); return false; }
    const auto groupPrefix = selected->groupName.isNotEmpty() ? selected->groupName + ": " : juce::String();
    detail << "  |  " << groupPrefix << selected->name;
    const auto modifierIt = std::find_if(event.stateModifiers.begin(),event.stateModifiers.end(),
        [&](const auto& modifier) { return bnkstate::matches(modifier,*selected); });
    if (modifierIt == event.stateModifiers.end() || !modifierIt->hasVolume)
        return true;

    const auto gain = std::pow(10.0f, modifierIt->volumeDb / 20.0f);
    if (!std::isfinite(gain)) { fail(work, Policy::Reason::parserBounds); return false; }
    for (int offset = 0; offset < decoded.buffer.getNumSamples(); offset += 4096)
    {
        if (!proceed(work)) return false;
        decoded.buffer.applyGain(offset, juce::jmin(4096, decoded.buffer.getNumSamples() - offset), gain);
    }
    diagnostics.add("Applied state modifier volume " + juce::String(modifierIt->volumeDb, 1) + " dB.");
    return true;
}

bool validatePreviewPcm(const DecodedMedia& decoded, const BackendWork& work)
{
    for (int offset = 0; offset < decoded.buffer.getNumSamples(); offset += 4096)
    {
        if (!proceed(work)) return false;
        const auto end = juce::jmin(offset + 4096, decoded.buffer.getNumSamples());
        for (int channel = 0; channel < decoded.buffer.getNumChannels(); ++channel)
        {
            const auto* samples = decoded.buffer.getReadPointer(channel);
            for (int i = offset; i < end; ++i)
                if (!std::isfinite(samples[i])) { fail(work, Policy::Reason::cacheInvalid); return false; }
        }
    }
    return true;
}

std::shared_ptr<SingleTrackDocument::State> buildPreviewState(const BnkDocument::State& bankState,
                                                              const BnkDocument::EventDescriptor& event,
                                                              DecodedMedia&& decoded)
{
    auto state = std::make_shared<SingleTrackDocument::State>();
    state->info.file = bankState.info.file;
    state->info.formatName = "Wwise Event";
    state->info.metadata.set("BNK Event", event.displayName);
    state->info.metadata.set("BNK Playback", event.playbackSummary);
    state->info.sampleRate = decoded.sampleRate;
    state->info.numChannels = decoded.buffer.getNumChannels();
    state->info.channelLayout = ChannelLayoutCatalog::defaultLayoutForChannelCount(state->info.numChannels);
    state->info.bitsPerSample = decoded.bitsPerSample;
    state->info.numSamples = decoded.buffer.getNumSamples();
    state->info.durationSeconds = decoded.sampleRate > 0.0
                                      ? static_cast<double>(decoded.buffer.getNumSamples()) / decoded.sampleRate
                                      : 0.0;
    state->audioBuffer = std::make_shared<juce::AudioBuffer<float>>(std::move(decoded.buffer));
    state->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::inMemoryBuffer;
    // The complete private PCM is already admitted. Its playback lifetime does
    // not depend on the bank locator remaining present; original source/Save
    // authority still uses the separately captured physical source identity.
    state->info.sourceAdmitted = true;
    state->playbackAvailability = SingleTrackDocument::PlaybackAvailability::randomAccess();
    state->envelopePoints = {
        { 0.0, 1.0f },
        { state->info.durationSeconds, 1.0f }
    };
    return state;
}
} // namespace

BnkPreparedPlayback BnkWemPlaybackBackend::preparePreview(const BnkDocument::State& state,
                                                          const BnkDocument::EventDescriptor& event,
                                                          const BnkPlaybackContext& context, media::OptionalMediaOperation::Ptr operation)
try
{
    BnkPreparedPlayback prepared;
    const BackendWork work { operation, resolvedInputs };
    if (!proceed(work)) return prepared;
    if (operation && !state.isSourceCurrent())
    { fail(work, Policy::Reason::ioFailure, Policy::Stage::admission); return prepared; }

    if (context.stateValueToken.isNotEmpty()
        && (event.stateTargetScope == BnkDocument::EventDescriptor::StateTargetScope::unavailable || !event.stateModifiersResolved))
    {
        prepared.detail = "State preview is unavailable because the event target or group/value reference is unresolved.";
        fail(work, Policy::Reason::parserBounds, Policy::Stage::admission);
        return prepared;
    }

    std::optional<DecodedMedia> decoded;
    switch (event.playbackHint)
    {
        case BnkDocument::PlaybackHint::single:
            if (!event.media.empty())
                decoded = decodeMedia(state, event.media.front(), mediaResolveMode, prepared.diagnostics, work);
            break;
        case BnkDocument::PlaybackHint::random:
            decoded = renderRandom(state, event, mediaResolveMode, prepared.diagnostics, work);
            break;
        case BnkDocument::PlaybackHint::sequence:
            decoded = renderSequence(state, event, mediaResolveMode, prepared.diagnostics, work);
            break;
        case BnkDocument::PlaybackHint::switchDefault:
            decoded = renderSwitchDefault(state, event, context, mediaResolveMode, prepared.diagnostics, work);
            break;
        case BnkDocument::PlaybackHint::layered:
            decoded = renderLayered(state, event, mediaResolveMode, prepared.diagnostics, work);
            break;
        case BnkDocument::PlaybackHint::unavailable:
        default:
            prepared.diagnostics.add("This BNK event shape is not playable by the current preview backend yet.");
            break;
    }

    if (!decoded.has_value())
    {
        fail(work, Policy::Reason::parserBounds);
        return prepared;
    }

    prepared.detail = event.displayName + "  |  " + event.playbackSummary;
    if (event.playbackHint == BnkDocument::PlaybackHint::switchDefault && context.switchValueToken.isNotEmpty())
    {
        const auto found = std::find_if(state.switchValues.begin(),
                                        state.switchValues.end(),
                                        [&context](const BnkDocument::SwitchValueDescriptor& descriptor)
                                        {
                                            return descriptor.token == context.switchValueToken;
                                        });
        if (found != state.switchValues.end())
        {
            const auto groupPrefix = found->groupName.isNotEmpty() ? (found->groupName + ": ") : juce::String();
            prepared.detail << "  |  " << groupPrefix << found->name;
        }
    }

    if (!applyStateModifiers(*decoded, event, context, state, prepared.detail, prepared.diagnostics, work)
        || !validatePreviewPcm(*decoded, work)) return prepared;
    if (operation && !state.isSourceCurrent())
    { fail(work, Policy::Reason::ioFailure, Policy::Stage::admission); return prepared; }

    prepared.state = buildPreviewState(state, event, std::move(*decoded));
    return prepared;
}
catch (const std::bad_alloc&)
{
    if (operation) operation->fail(Policy::Reason::memoryQuota, Policy::Stage::decode);
    BnkPreparedPlayback failed;
    failed.detail = "Insufficient memory for the bounded BNK preview.";
    return failed;
}

BnkPreparedPlayback BnkWemPlaybackBackend::prepareMediaPreview(const BnkDocument::State& state,
                                                               const BnkDocument::PlayableMediaDescriptor& media, media::OptionalMediaOperation::Ptr operation)
try
{
    BnkPreparedPlayback prepared;
    const BackendWork work { operation, resolvedInputs };
    if (!proceed(work)) return prepared;
    if (operation && !state.isSourceCurrent())
    { fail(work, Policy::Reason::ioFailure, Policy::Stage::admission); return prepared; }
    auto decoded = decodeMedia(state, media, mediaResolveMode, prepared.diagnostics, work);
    if (!decoded.has_value())
    {
        prepared.diagnostics.add("This BNK media entry is not directly playable yet.");
        return prepared;
    }

    BnkDocument::EventDescriptor pseudoEvent;
    pseudoEvent.id = media.mediaId;
    pseudoEvent.displayName = media.displayName;
    pseudoEvent.playbackSummary = media.extractableFromBank ? "Raw audio preview (embedded media)" : "Raw audio preview";
    prepared.detail = pseudoEvent.displayName + "  |  " + pseudoEvent.playbackSummary;
    if (!validatePreviewPcm(*decoded, work)) return prepared;
    if (operation && !state.isSourceCurrent())
    { fail(work, Policy::Reason::ioFailure, Policy::Stage::admission); return prepared; }
    prepared.state = buildPreviewState(state, pseudoEvent, std::move(*decoded));
    return prepared;
}
catch (const std::bad_alloc&)
{
    if (operation) operation->fail(Policy::Reason::memoryQuota, Policy::Stage::decode);
    BnkPreparedPlayback failed;
    failed.detail = "Insufficient memory for the bounded BNK preview.";
    return failed;
}

BnkPreviewRequest::Ptr BnkPreviewRequest::forEvent(BnkDocument::StatePtr bank, uint32_t id,
                                                  BnkPlaybackContext context, BnkMediaResolveMode mode)
{
    if (!bank) return {};
    const auto found = std::find_if(bank->events.begin(), bank->events.end(), [id](const auto& event) { return event.id == id; });
    if (found == bank->events.end()) return {};
    auto request = std::shared_ptr<BnkPreviewRequest>(new BnkPreviewRequest);
    request->eventIndex = static_cast<size_t>(std::distance(bank->events.begin(), found));
    request->bank = std::move(bank);
    request->context = std::move(context); request->mode = mode;
    if (found->playbackHint == BnkDocument::PlaybackHint::random)
        request->randomSeed = juce::Random::getSystemRandom().nextInt64();
    return request;
}
BnkPreviewRequest::Ptr BnkPreviewRequest::forMedia(BnkDocument::StatePtr bank,
    BnkDocument::PlayableMediaDescriptor media, BnkMediaResolveMode mode)
{
    if (!bank || media.mediaId == 0) return {};
    auto request = std::shared_ptr<BnkPreviewRequest>(new BnkPreviewRequest);
    request->bank = std::move(bank); request->rawMedia = std::move(media); request->mode = mode;
    return request;
}
BnkPreparedPlayback BnkPreviewRequest::prepare(const media::OptionalMediaOperation::Ptr& operation) const
try
{
    BnkPreparedPlayback failed;
    if (!operation || !operation->beginBankPreparation()) return failed;
    const auto checkpoint = [&] { return operation->continueWork(Policy::Stage::admission); };
    const auto reject = [&](Policy::Reason reason)
    { operation->fail(reason, Policy::Stage::admission); return failed; };
    if (!bank || !checkpoint() || !bank->isSourceCurrent()) return reject(Policy::Reason::ioFailure);
    if (mode != BnkMediaResolveMode::decodePackedMedia && mode != BnkMediaResolveMode::sourceMedia)
        return reject(Policy::Reason::parserBounds);

    // Physical read leases cover the bank, producer and every selected external
    // source until materialisation finishes. They are not original Save rights.
    std::vector<std::unique_ptr<media::SourceFileLease>> leases;
    juce::MemoryOutputStream scope;
    const auto token = [&](const juce::String& text)
    {
        const auto hash = juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).getRawData();
        scope.write(hash.getData(), hash.getSize());
    };
    const auto pin = [&](const juce::File& file)
    {
        if (!checkpoint()) return false;
        std::unique_ptr<media::SourceFileLease> lease;
        if (media::SourceFileLease::open(file, lease).failed()) return false;
        const auto facts = lease->witness();
        token(file.getFullPathName()); token(facts->volumeId); token(facts->fileId);
        scope.writeInt64(facts->sizeBytes); scope.writeInt64(facts->modifiedTicks); scope.writeInt64(facts->changedTicks);
        leases.push_back(std::move(lease));
        return true;
    };
    token("bnk-resolved-request/v4");
    if (!pin(bank->info.file) || !bank->isSourceCurrent()) return reject(Policy::Reason::ioFailure);
    const auto producer = VgmstreamDecodeService::getCliPath();
    if (producer.existsAsFile() && !pin(producer)) return reject(Policy::Reason::ioFailure);
    scope.writeBool(rawMedia.has_value()); scope.writeInt(static_cast<int>(mode));
    token(context.switchValueToken); token(context.stateValueToken); token(context.rtpcToken);
    scope.writeFloat(context.rtpcValue);
    if (!std::isfinite(context.rtpcValue)) return reject(Policy::Reason::parserBounds);

    const BnkDocument::EventDescriptor emptyEvent;
    const auto& event = eventIndex ? bank->events[*eventIndex] : emptyEvent;
    auto renderedEvent = event;
    auto renderedRaw = rawMedia;
    std::vector<size_t> selected;
    if (!rawMedia)
    {
        scope.writeInt(static_cast<int>(event.id)); scope.writeInt(static_cast<int>(event.playbackHint));
        token(event.authoringId); token(event.targetObjectId); token(event.targetObjectName);
        scope.writeInt(static_cast<int>(event.stateTargetScope));
        scope.writeBool(event.stateModifiersResolved);
        if (bank->stateValues.size() > 4096) return reject(Policy::Reason::parserBounds);
        for (const auto& value : bank->stateValues)
        {
            if (!checkpoint()) return failed;
            token(bnkstate::token(value)); token(value.name); token(value.groupName);
        }
        using Hint = BnkDocument::PlaybackHint;
        if (event.media.empty() || event.media.size() > 65536 || event.stateModifiers.size() > 4096)
            return reject(Policy::Reason::parserBounds);
        if (event.playbackHint == Hint::single) selected.push_back(0);
        else if (event.playbackHint == Hint::sequence || event.playbackHint == Hint::layered)
        {
            const auto limit = event.playbackHint == Hint::sequence ? 16u : 8u;
            if (event.media.size() > limit) return reject(Policy::Reason::parserBounds);
            for (size_t i = 0; i < event.media.size(); ++i) selected.push_back(i);
        }
        else if (event.playbackHint == Hint::random)
        {
            for (size_t i = 0; i < event.media.size(); ++i)
            {
                if (!checkpoint()) return failed;
                if (event.media[i].mediaFile.existsAsFile() || event.media[i].sourceFile.existsAsFile()
                    || event.media[i].shortName.isNotEmpty()) selected.push_back(i);
            }
            if (selected.empty()) return reject(Policy::Reason::parserBounds);
            juce::Random random(randomSeed);
            const auto chosen = selected[static_cast<size_t>(random.nextInt(static_cast<int>(selected.size())))];
            selected = { chosen };
        }
        else if (event.playbackHint == Hint::switchDefault)
        {
            std::optional<size_t> explicitChoice, defaultChoice, fallback;
            for (size_t i = 0; i < event.media.size(); ++i)
            {
                if (!checkpoint()) return failed;
                const auto& input = event.media[i];
                if (!explicitChoice && context.switchValueToken.isNotEmpty() && input.switchValueToken == context.switchValueToken) explicitChoice = i;
                if (!defaultChoice && input.isDefaultSwitch) defaultChoice = i;
            if (!fallback && (input.mediaFile.existsAsFile() || input.sourceFile.existsAsFile() || input.shortName.isNotEmpty())) fallback = i;
            }
            const auto chosen = explicitChoice ? explicitChoice : defaultChoice ? defaultChoice : fallback;
            if (!chosen) return reject(Policy::Reason::parserBounds);
            selected = { *chosen };
        }
        else return reject(Policy::Reason::parserBounds);
        renderedEvent.media.clear();
        for (const auto i : selected) { scope.writeInt64(static_cast<int64_t>(i)); renderedEvent.media.push_back(event.media[i]); }
        for (const auto& modifier : event.stateModifiers)
        {
            if (!checkpoint()) return failed;
            token(modifier.stateValueToken); token(modifier.stateName);
            token(bnkstate::group(modifier));
            scope.writeBool(modifier.hasVolume); scope.writeFloat(modifier.volumeDb);
        }
    }
    else scope.writeInt(static_cast<int>(rawMedia->mediaId));

    const BackendWork preparation { operation, false };
    const auto resolve = [&](const PlaybackMediaTarget& input) -> juce::File
    {
        if (mode == BnkMediaResolveMode::sourceMedia)
        {
            const auto original = tryFindOriginalMedia(*bank, input, preparation);
            if (original.existsAsFile()) return original;
        }
        if (!checkpoint()) return {};
        if (input.mediaFile.existsAsFile()) return input.mediaFile;
        if (input.mediaId != 0)
        {
            const auto generated = bank->info.file.getParentDirectory().getChildFile(juce::String(input.mediaId) + ".wem");
            if (generated.existsAsFile()) return generated;
        }
        if (input.allowBankExtract && std::any_of(bank->mediaEntries.begin(), bank->mediaEntries.end(),
            [&](const auto& entry) { return entry.id == input.mediaId; })) return {};
        return mode == BnkMediaResolveMode::decodePackedMedia
            ? tryFindOriginalMedia(*bank, input, preparation) : juce::File();
    };
    bool unresolved = false;
    const auto freezeInput = [&](PlaybackMediaTarget input, juce::File& resolvedFile)
    {
        if (!checkpoint()) return false;
        scope.writeInt(static_cast<int>(input.mediaId));
        resolvedFile = resolve(input);
        if (!checkpoint()) return false;
        if (resolvedFile != juce::File()) { scope.writeInt(1); return pin(resolvedFile); }
        const auto embedded = std::find_if(bank->mediaEntries.begin(), bank->mediaEntries.end(),
            [&](const auto& entry) { return entry.id == input.mediaId; });
        if (input.allowBankExtract && embedded != bank->mediaEntries.end())
        {
            scope.writeInt(2); scope.writeInt64(bank->dataChunkStart); scope.writeInt64(bank->dataChunkSize);
            scope.writeInt(static_cast<int>(embedded->offset)); scope.writeInt(static_cast<int>(embedded->size));
            return true;
        }
        scope.writeInt(0); token(input.mediaFile.getFullPathName()); token(input.relativePath); token(input.shortName);
        unresolved = true;
        return true;
    };
    if (renderedRaw)
    {
        const auto& input = *rawMedia;
        if (!freezeInput({ input.mediaId, input.displayName, input.shortName, input.relativePath, input.mediaFile, input.extractableFromBank, input.sourceFile }, renderedRaw->mediaFile))
            return reject(Policy::Reason::ioFailure);
    }
    else for (auto& input : renderedEvent.media)
        if (!freezeInput({ input.mediaId, input.displayName, input.shortName, input.relativePath, input.mediaFile, true, input.sourceFile }, input.mediaFile))
            return reject(Policy::Reason::ioFailure);
    if (!checkpoint()) return failed;
    const auto scopeHash = juce::SHA256(scope.getData(), scope.getDataSize()).toHexString();
    if (!operation->admitBank(bank->info.file, producer, scopeHash)) return failed;
    if (unresolved) return reject(Policy::Reason::ioFailure);
    for (const auto& lease : leases) if (lease->recheck().failed()) return reject(Policy::Reason::ioFailure);
    BnkWemPlaybackBackend backend;
    backend.resolvedInputs = true;
    backend.setMediaResolveMode(mode);
    auto prepared = renderedRaw ? backend.prepareMediaPreview(*bank, *renderedRaw, operation)
                               : backend.preparePreview(*bank, renderedEvent, context, operation);
    if (!operation->continueWork(Policy::Stage::admission)) return failed;
    for (const auto& lease : leases) if (lease->recheck().failed()) return reject(Policy::Reason::ioFailure);
    return prepared; // Actual native/current-Document admission still owns succeed.
}
catch (const std::bad_alloc&)
{
    if (operation) operation->fail(Policy::Reason::memoryQuota, Policy::Stage::admission);
    return {};
}
} // namespace singleeditor
