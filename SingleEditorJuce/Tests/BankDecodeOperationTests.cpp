#include "Audio/VgmstreamDecodeService.h"
#include "Bnk/BnkParser.h"
#include "Bnk/BnkWemPlaybackBackend.h"
#include "Bnk/BnkPreviewRequest.h"
#include "DecoderProcessWitness.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
using namespace singleeditor;
using namespace singleeditor::media;
using Policy = OptionalMediaFailureState;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
juce::File gate(const juce::File& file) { return file.getSiblingFile(file.getFileName() + ".finish"); }
juce::File pids(const juce::File& file) { return file.getSiblingFile(file.getFileName() + ".pids"); }
int invocations(const juce::File& file)
{ auto lines = juce::StringArray::fromLines(pids(file).loadFileAsString()); lines.removeEmptyStrings(); return lines.size(); }
template<class F> void await(F condition)
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (!condition() && juce::Time::getMillisecondCounterHiRes() - start < 5000) juce::Thread::sleep(5);
    require(condition(), "actual bank decoder child started");
}
void exited(const juce::File& file)
{
#if JUCE_WINDOWS
    for (const auto& line : juce::StringArray::fromLines(pids(file).loadFileAsString()))
    {
        if (line.isEmpty()) continue;
        const auto done = tests::decoderProcessExited(line);
        if (!done)
            std::cerr << "Bank decoder still live or unverified: " << file.getFullPathName() << " [" << line << "]\n";
        require(done, "actual bank decoder process identity has exited");
    }
#endif
}
juce::String lockName(const juce::File& wave)
{
    const auto path = wave.getFullPathName().toLowerCase();
    return "SingleEditor-vgmstream-" + juce::SHA256(path.toRawUTF8(), path.getNumBytesAsUTF8()).toHexString();
}
}

// Private, external real-media qualification. The manifest and audio remain
// outside the source tree; this path never substitutes the controlled decoder.
void runRealBankCharacterization(const juce::File& root, bool bankMode)
{
    const auto manifest = juce::JSON::parse(root.getChildFile("manifest.json"));
    require(manifest.isObject(), "read explicit private real-media manifest");
    const auto fixtureRoot = root.getChildFile("fixtures");
    const auto file = [&](const char* field)
    {
        const auto record = manifest.getProperty(field, {});
        const auto value = fixtureRoot.getChildFile(record.getProperty("path", {}).toString());
        require(value.isAChildOf(fixtureRoot) && value.existsAsFile(), "fixture remains within private copied root");
        require(juce::SHA256(value).toHexString().equalsIgnoreCase(record.getProperty("sha256", {}).toString()),
                "real fixture bytes match frozen identity");
        return value;
    };
    const auto bank = file("bank"), wem = file("media"), reference = file("reference");
    if (manifest.hasProperty("sidecar")) static_cast<void>(file("sidecar"));
    const bool embedded = manifest.getProperty("storage", "external").toString() == "embedded";
    const auto projectRoot = fixtureRoot.getChildFile(manifest.getProperty("project_root", ".").toString());
    require(projectRoot == fixtureRoot || projectRoot.isAChildOf(fixtureRoot), "explicit bank scan root remains inside fixture");
    require(VgmstreamDecodeService::getCliPath().existsAsFile(), "actual packaged decoder is present");
    const bool needsDecoder = VgmstreamDecodeService::chooseWemOpenStrategy(wem).kind
                              == VgmstreamDecodeService::WemOpenStrategy::Kind::decodedFallback;
    require(needsDecoder == static_cast<bool>(manifest.getProperty("requires_decoder", true)),
            "real fixture uses its declared direct-PCM or compressed fallback path");
    auto parsed = BnkParser::parse(bank, projectRoot);
    juce::DynamicObject::Ptr observation(new juce::DynamicObject());
    observation->setProperty("schema", "singleeditor.private-real-bank-characterization");
    observation->setProperty("version", 1);
    observation->setProperty("mode", bankMode ? "bank" : "standalone-wem");
    observation->setProperty("storage", embedded ? "embedded" : "external");
    observation->setProperty("requires_decoder", needsDecoder);
    observation->setProperty("bank_sha256", juce::SHA256(bank).toHexString());
    observation->setProperty("media_sha256", juce::SHA256(wem).toHexString());
    observation->setProperty("reference_sha256", juce::SHA256(reference).toHexString());
    observation->setProperty("decoder_path", VgmstreamDecodeService::getCliPath().getFullPathName());
    observation->setProperty("decoder_sha256", juce::SHA256(VgmstreamDecodeService::getCliPath()).toHexString());
    observation->setProperty("parse_ok", static_cast<bool>(parsed));
    observation->setProperty("failure", static_cast<int>(parsed.failure));
    juce::Array<juce::var> eventRows;
    if (parsed.state)
    {
        observation->setProperty("bank_version", static_cast<int>(parsed.state->info.version));
        observation->setProperty("object_count", static_cast<int>(parsed.state->objects.size()));
        observation->setProperty("diagnostics", parsed.state->info.diagnostics.joinIntoString("\n"));
        for (const auto& event : parsed.state->events)
        {
            auto row = std::make_unique<juce::DynamicObject>();
            row->setProperty("id", static_cast<juce::int64>(event.id));
            row->setProperty("summary", event.summary);
            row->setProperty("playback_summary", event.playbackSummary);
            juce::Array<juce::var> mediaRows;
            for (const auto& item : event.media)
            {
                auto media = std::make_unique<juce::DynamicObject>();
                media->setProperty("id", static_cast<juce::int64>(item.mediaId));
                media->setProperty("path", item.mediaFile.getFullPathName());
                mediaRows.add(juce::var(media.release()));
            }
            row->setProperty("media", mediaRows);
            eventRows.add(juce::var(row.release()));
        }
    }
    observation->setProperty("events", eventRows);
    const auto report = root.getChildFile(bankMode ? "qualification.json" : "wem-qualification.json");
    require(!report.exists(), "preserve every prior real-media qualification");
    const auto writeReport = [&] { require(report.replaceWithText(juce::JSON::toString(juce::var(observation.get()))),
                                         "retain actual real-bank observation"); };
    writeReport();
    require(static_cast<bool>(parsed), "existing parser accepts the real bank");
    auto cache = embedded ? juce::File() : VgmstreamDecodeService::getDecodedWaveFile(wem);
    if (!embedded)
    {
        require(!cache.exists() && !DecodedWaveCache::sealFile(cache).exists(), "fresh private source has no prior decoded cache");
        observation->setProperty("decoded_cache", cache.getFullPathName());
    }
    juce::File extractedFile, embeddedDirectory;
    juce::AudioBuffer<float> pcm;
    double sampleRate = 0;
    if (bankMode)
    {
        BnkDocument document;
        require(document.adoptParsedState(parsed.state), "adopt real parsed bank");
        const auto mediaId = static_cast<uint32_t>(static_cast<juce::int64>(manifest.getProperty("media_id", {})));
        const auto descriptor = document.getPlayableMediaById(mediaId);
        require(descriptor.has_value(), "real bank exposes selected actual media id");
        if (embedded)
        {
            require(!descriptor->mediaFile.existsAsFile() && !descriptor->sourceFile.existsAsFile()
                        && descriptor->extractableFromBank, "selected audio must come from genuine bank DATA, not the oracle copy");
            const auto identity = bank.getFullPathName() + "|" + juce::String(bank.getSize()) + "|"
                                  + juce::String(bank.getLastModificationTime().toMilliseconds());
            embeddedDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                .getChildFile("SingleEditor/bnk_media_cache")
                .getChildFile(juce::String::toHexString(static_cast<juce::int64>(identity.hashCode64())));
            require(!embeddedDirectory.exists(), "new private bank has no previous embedded extraction");
            extractedFile = embeddedDirectory.getChildFile(juce::String(mediaId) + ".wem");
            observation->setProperty("extracted_media", extractedFile.getFullPathName());
        }
        else require(descriptor->mediaFile == wem, "real bank resolves the exact external WEM");
        auto request = BnkPreviewRequest::forMedia(parsed.state, *descriptor, BnkMediaResolveMode::decodePackedMedia);
        require(request != nullptr, "create frozen real-media request");
        auto operation = std::make_shared<OptionalMediaOperation>(std::make_shared<Policy>(), Policy::Intent::initial,
                                                               std::nullopt, Policy::Module::bank);
        auto preview = request->prepare(operation);
        observation->setProperty("preview_valid", preview.isValid());
        observation->setProperty("preview_detail", preview.detail);
        observation->setProperty("preview_diagnostics", preview.diagnostics.joinIntoString("\n"));
        writeReport();
        require(preview.isValid() && !operation->report().failure, "actual admitted real WEM decode succeeds");
        if (embedded)
        {
            require(extractedFile.existsAsFile() && juce::SHA256(extractedFile) == juce::SHA256(wem),
                    "product extraction exactly matches independently read real DIDX region");
            if (needsDecoder)
            {
                require(operation->report().process.has_value() && operation->report().process->jobCleanupConfirmed,
                        "new embedded fixture completes its actual bounded decoder process rather than borrowing a cache");
                cache = VgmstreamDecodeService::getDecodedWaveFile(extractedFile);
                require(cache.existsAsFile() && DecodedWaveCache::sealFile(cache).existsAsFile(),
                        "actual embedded decode publishes its complete cache and seal");
                observation->setProperty("decoded_cache", cache.getFullPathName());
            }
        }
        pcm.makeCopyOf(*preview.state->audioBuffer);
        sampleRate = preview.state->info.sampleRate;
    }
    else
    {
        juce::StringArray diagnostics;
        auto decoded = VgmstreamDecodeService::createReaderForDecodedFile(wem, &diagnostics);
        observation->setProperty("decoded_reader_valid", decoded != nullptr);
        observation->setProperty("decode_diagnostics", diagnostics.joinIntoString("\n"));
        writeReport();
        require(decoded && decoded->lengthInSamples > 0 && decoded->lengthInSamples <= 48000 * 10,
                "real compressed qualification has a complete bounded reader");
        pcm.setSize(static_cast<int>(decoded->numChannels), static_cast<int>(decoded->lengthInSamples));
        require(decoded->read(&pcm, 0, pcm.getNumSamples(), 0, true, true), "read every actual compressed-media frame");
        sampleRate = decoded->sampleRate;
    }
    writeReport();
    juce::WavAudioFormat format;
    auto reader = std::unique_ptr<juce::AudioFormatReader>(format.createReaderFor(reference.createInputStream().release(), true));
    const auto referenceFormat = manifest.getProperty("reference_format", "float32").toString();
    require(reader && ((referenceFormat == "float32" && reader->usesFloatingPointData && reader->bitsPerSample == 32)
                       || (referenceFormat == "pcm16" && !reader->usesFloatingPointData && reader->bitsPerSample == 16)),
            "reference encoding exactly matches the explicitly frozen decode mode");
    observation->setProperty("reference_format", referenceFormat);
    observation->setProperty("reference_provenance", manifest.getProperty("reference_provenance", {}));
    require(reader->sampleRate == sampleRate && reader->numChannels == static_cast<unsigned>(pcm.getNumChannels())
                && reader->lengthInSamples == pcm.getNumSamples(), "complete sample rate, channel and frame identity");
    juce::AudioBuffer<float> expected(pcm.getNumChannels(), pcm.getNumSamples());
    require(reader->read(&expected, 0, expected.getNumSamples(), 0, true, true), "read entire independently retained reference");
    float maxError = 0.0f;
    for (int ch = 0; ch < pcm.getNumChannels(); ++ch)
        for (int frame = 0; frame < pcm.getNumSamples(); ++frame)
        {
            const auto actual = pcm.getSample(ch, frame), golden = expected.getSample(ch, frame);
            require(std::isfinite(actual) && std::isfinite(golden), "all actual and reference samples are finite");
            maxError = std::max(maxError, std::abs(actual - golden));
        }
    observation->setProperty("frames", pcm.getNumSamples());
    observation->setProperty("channels", pcm.getNumChannels());
    observation->setProperty("sample_rate", reader->sampleRate);
    observation->setProperty("maximum_absolute_error", maxError);
    observation->setProperty("pcm_exact", maxError == 0.0f);
    writeReport();
    require(maxError == 0.0f, "all decoded PCM samples exactly match retained real-media reference");
    if (cache != juce::File())
    {
        juce::InterProcessLock cacheLock(lockName(cache));
        require(cacheLock.enter(5000) && cache.getParentDirectory() == VgmstreamDecodeService::getDecodeCacheDirectory(),
                "exact new private cache producer is released");
        require(cache.deleteFile() && DecodedWaveCache::sealFile(cache).deleteFile(), "remove only this invocation's exact decoded files");
    }
    if (embedded)
        require(extractedFile.deleteFile() && embeddedDirectory.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0
                    && embeddedDirectory.deleteFile(), "remove only exact new extraction and its now-empty private directory");
    observation->setProperty("owned_cache_removed", true);
    observation->setProperty("status", bankMode ? "bank-positive-qualified" : "standalone-wem-positive-qualified");
    writeReport();
    std::cout << (bankMode ? "Real bank" : "Standalone real WEM")
              << " positive qualification passed; native lifecycle and refactor start gate remain separate\n";
}

// Complete-reader transport under one already-admitted bank operation. This
// exercises real bounded children/cache locks, not the future native bank UI.
void runBankDecodeOperationTests()
{
#if JUCE_WINDOWS
    const auto current = tests::currentDecoderProcessWitness();
    require(current.isNotEmpty() && !tests::decoderProcessExited(current), "matching live process identity must fail the exit check");
    const auto changedIdentity = juce::String(static_cast<juce::int64>(GetCurrentProcessId())) + " "
        + juce::String(tests::creationStamp(GetCurrentProcess()) - 1);
    require(tests::decoderProcessExited(changedIdentity), "later process reusing a PID is not the recorded child");
    require(!tests::decoderProcessExited("123") && !tests::decoderProcessExited("123 invalid"), "missing process identity cannot establish child exit");
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-BankDecode-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private bank decode fixture");
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto previous = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_VGMSTREAM_PATH", {});
    require(_wputenv_s(L"SINGLE_EDITOR_VGMSTREAM_PATH", executable.getFullPathName().toWideCharPointer()) == 0, "set only test-process decoder");
    const auto bank = root.getChildFile("scope.bnk");
    juce::MemoryOutputStream bankBytes;
    bankBytes.write("BKHD", 4); bankBytes.writeInt(8); bankBytes.writeInt(128); bankBytes.writeInt(7);
    require(bank.replaceWithData(bankBytes.getData(), bankBytes.getDataSize()), "create physical bounded bank fixture");
    const auto bankState = BnkParser::parseFile(bank, root);
    require(bankState && bankState->isSourceCurrent(), "parse actual bank source witness for backend requests");
    const auto session = std::make_shared<Policy>();
    const auto make = [&](const char* name, const char* mode, bool complete)
    {
        const auto input = root.getChildFile(name);
        require(input.replaceWithText(juce::String("packed-fixture:") + mode), "create private actual decoder input");
        if (complete) require(gate(input).replaceWithText("finish"), "release private decoder fixture");
        return input;
    };
    const auto operation = [&](const juce::String& scope, Policy::Intent intent = Policy::Intent::initial, std::optional<Policy::Key> key = {})
    {
        auto result = std::make_shared<OptionalMediaOperation>(session, intent, key, Policy::Module::bank);
        require(result->admitBank(bank, executable, scope), "admit exact bank work scope");
        return result;
    };
    const auto success = make("success.wem", "success", true);
    {
        auto work = operation("success");
        auto reader = VgmstreamDecodeService::createReaderForDecodedFile(success, nullptr, work);
        juce::AudioBuffer<float> pcm(1, 64);
        require(reader && reader->lengthInSamples == 144000 && reader->read(&pcm, 0, 64, 96000, true, false)
            && pcm.getSample(0, 0) == 0.25f && !work->report().terminal, "one real complete reader cannot finish its enclosing bank operation");
        require(work->report().cache.lookupReason == DecodedWaveCache::LookupReason::entryUnavailable
            && work->report().cache.lookups == 1 && work->report().cache.publications == 1,
            "real first bank decode retains the original miss reason after publication");
        reader.reset();
        reader = VgmstreamDecodeService::createReaderForDecodedFile(success, nullptr, work);
        require(reader && invocations(success) == 1 && work->continueWork(Policy::Stage::decode), "same admitted operation reuses sealed complete cache without another child");
        require(work->report().cache.lookupReason == DecodedWaveCache::LookupReason::reused
            && work->report().cache.lookups == 2 && work->report().cache.hits == 1,
            "real second bank lookup replaces the bounded reason with its actual hit");
        reader.reset(); work->succeed(); require(work->report().succeeded, "caller finishes only its complete composite work");
    }
    const auto failure = make("failure.wem", "late-exit", true);
    {
        auto first = operation("failure");
        require(!VgmstreamDecodeService::createReaderForDecodedFile(failure, nullptr, first)
            && first->report().failure && first->report().failure->exitCode == 37, "actual complete decoder failure reaches bank policy");
        auto retry = operation("failure", Policy::Intent::retryOnce, first->report().key);
        require(!VgmstreamDecodeService::createReaderForDecodedFile(failure, nullptr, retry)
            && retry->policyView().circuitOpen && invocations(failure) == 2, "two actual matching bank failures open the session fuse");
        require(!VgmstreamDecodeService::getDecodedWaveFile(failure).existsAsFile(), "failed complete decode never publishes a partial cache");
    }
    const auto cancel = make("cancel.wem", "hang", false);
    {
        auto work = operation("cancel"); std::unique_ptr<juce::AudioFormatReader> result;
        std::thread worker([&] { result = VgmstreamDecodeService::createReaderForDecodedFile(cancel, nullptr, work); });
        await([&] { return invocations(cancel) == 1; }); work->requestCancel(); worker.join();
        require(!result && work->report().cancelled && work->report().process && work->report().process->jobCleanupConfirmed,
            "complete decode accepts real cancellation and confirms owned Job cleanup"); exited(cancel);
    }
    const auto waiting = make("waiting.wem", "success", true);
    {
        const auto wave = VgmstreamDecodeService::getDecodedWaveFile(waiting);
        juce::InterProcessLock holder(lockName(wave)); require(holder.enter(0), "hold actual cache producer mutex on this thread");
        auto work = operation("waiting"); std::unique_ptr<juce::AudioFormatReader> result;
        std::thread worker([&] { result = VgmstreamDecodeService::createReaderForDecodedFile(waiting, nullptr, work); });
        juce::Thread::sleep(100); work->requestCancel(); worker.join();
        require(!result && work->report().cancelled && !work->report().process && invocations(waiting) == 0 && !wave.exists(),
            "cancelled complete-cache waiter neither launches child nor acquires delete permission");
    }
    const auto quota = make("quota.wem", "quota", true);
    {
        auto first = operation("quota");
        require(!VgmstreamDecodeService::createReaderForDecodedFile(quota, nullptr, first)
            && first->report().failure && first->report().failure->reason == Policy::Reason::outputQuota, "actual sparse output enforces conservative bank quota");
        auto expanded = operation("quota", Policy::Intent::expandBudget, first->report().key);
        auto reader = VgmstreamDecodeService::createReaderForDecodedFile(quota, nullptr, expanded);
        require(reader && reader->lengthInSamples == 144000 && invocations(quota) == 2, "explicit bounded bank expansion accepts valid complete audio");
        reader.reset(); expanded->succeed();
    }
    const auto deadlineFirst = make("deadline-first.wem", "success", true);
    const auto deadlineSecond = make("deadline-second.wem", "hang", false);
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        auto work = operation("two-step-deadline");
        auto reader = VgmstreamDecodeService::createReaderForDecodedFile(deadlineFirst, nullptr, work);
        require(reader != nullptr, "first real bank member completes"); reader.reset();
        juce::Thread::sleep(200);
        const auto remaining = work->remainingWallTimeMs();
        require(remaining < 29800 && remaining > 0, "first member consumes the same whole-operation time budget");
        reader = VgmstreamDecodeService::createReaderForDecodedFile(deadlineSecond, nullptr, work);
        require(!reader && work->report().failure && work->report().failure->reason == Policy::Reason::deadline
            && juce::Time::getMillisecondCounterHiRes() - started < 33000,
            "second real decoder shares the original 30-second wall deadline instead of receiving a fresh budget"); exited(deadlineSecond);
    }
    {
        const auto wave = VgmstreamDecodeService::getDecodedWaveFile(success);
        const auto context = DecodedWaveCache::capture(success, executable, operation("hash-context")->bankPolicy());
        require(context.has_value(), "capture real completed cache for cancellable validation");
        juce::FileInputStream input(wave);
        const auto size = input.getTotalLength();
        auto work = operation("hash-cancel");
        const auto stopped = checkedStreamHash(input, size, [&]
        {
            if (input.getPosition() >= 64 * 1024) work->requestCancel();
            return work->continueWork(Policy::Stage::cache);
        });
        require(!stopped && work->report().cancelled && input.getPosition() == 64 * 1024 && input.getPosition() < size,
            "actual cache body read stops at the next 64 KiB boundary without accepting a prefix digest");
        require(input.setPosition(0), "rewind completed cache");
        const auto full = checkedStreamHash(input, size);
        require(full && *full == juce::SHA256(wave), "bounded buffered hashing preserves exact existing SHA256");
        const auto seal = DecodedWaveCache::sealFile(wave);
        const auto oldSeal = juce::SHA256(seal);
        int checks = 0;
        DecodedWaveCache::LookupReason reason;
        require(!DecodedWaveCache::reusable(*context, wave, [&] { return ++checks < 12; }, &reason)
            && reason == DecodedWaveCache::LookupReason::workEnded, "cancelled complete-cache body read reports ended work");
        checks = 0;
        require(!DecodedWaveCache::seal(*context, wave, [&] { return ++checks < 12; }) && juce::SHA256(seal) == oldSeal,
            "cancelled reseal cannot replace the existing complete seal");
        require(DecodedWaveCache::reusable(*context, wave), "read-only cancellation leaves the completed cache usable");
    }
    const auto backendFailure = make("backend-failure.wem", "late-exit", true);
    const auto alternative = make("777.wem", "success", true);
    const auto backendCancel = make("backend-cancel.wem", "hang", false);
    const auto changedBankSource = make("changed-bank.wem", "success", false);
    {
        BnkWemPlaybackBackend backend;
        BnkDocument::EventDescriptor event;
        event.id = 42; event.displayName = "actual composite fixture"; event.playbackHint = BnkDocument::PlaybackHint::sequence;
        BnkDocument::EventDescriptor::MediaReference good, bad;
        good.mediaId = 1; good.mediaFile = success;
        bad.mediaId = 777; bad.mediaFile = backendFailure;
        event.media = { good, good };
        auto complete = operation("backend-complete");
        auto preview = backend.preparePreview(*bankState, event, {}, complete);
        require(preview.isValid() && preview.state->info.numSamples == 288000
            && preview.state->audioBuffer->getSample(0, 144032) == 0.25f && !complete->report().terminal,
            "actual backend assembles the whole sequence and leaves final admission to its owner");
        preview = {}; complete->succeed();
        event.media = { good, bad };
        auto first = operation("backend-failure");
        preview = backend.preparePreview(*bankState, event, {}, first);
        require(!preview.isValid() && first->report().failure && first->report().failure->exitCode == 37
            && invocations(backendFailure) == 1 && invocations(alternative) == 0,
            "actual second-member failure publishes no partial sequence and never silently decodes the alternative location");
        auto retry = operation("backend-failure", Policy::Intent::retryOnce, first->report().key);
        preview = backend.preparePreview(*bankState, event, {}, retry);
        require(!preview.isValid() && retry->policyView().circuitOpen && invocations(backendFailure) == 2 && invocations(alternative) == 0,
            "actual composite retry has one exact attempt and opens the shared fuse after its second failure");
        bad.mediaId = 778; bad.mediaFile = backendCancel; event.media = { good, bad };
        auto cancelled = operation("backend-cancel");
        std::thread worker([&] { preview = backend.preparePreview(*bankState, event, {}, cancelled); });
        await([&] { return invocations(backendCancel) == 1; }); cancelled->requestCancel(); worker.join();
        require(!preview.isValid() && cancelled->report().cancelled && cancelled->report().process
            && cancelled->report().process->jobCleanupConfirmed,
            "actual second-member cancellation discards the already-read first member and drains its real child");
        exited(backendCancel);
        const auto invalidWave = root.getChildFile("nonfinite.wav");
        juce::MemoryOutputStream floats;
        floats.write("RIFF", 4); floats.writeInt(36 + 64 * 4); floats.write("WAVEfmt ", 8); floats.writeInt(16);
        floats.writeShort(3); floats.writeShort(1); floats.writeInt(48000); floats.writeInt(48000 * 4);
        floats.writeShort(4); floats.writeShort(32); floats.write("data", 4); floats.writeInt(64 * 4);
        for (int i = 0; i < 64; ++i) floats.writeFloat(i == 32 ? std::numeric_limits<float>::quiet_NaN() : 0.25f);
        require(invalidWave.replaceWithData(floats.getData(), floats.getDataSize()), "create actual nonfinite float-WAV input");
        BnkDocument::PlayableMediaDescriptor invalid;
        invalid.mediaId = 88; invalid.mediaFile = invalidWave;
        auto nonfinite = operation("nonfinite");
        preview = backend.prepareMediaPreview(*bankState, invalid, nonfinite);
        require(!preview.isValid() && nonfinite->report().failure && nonfinite->report().failure->reason == Policy::Reason::cacheInvalid,
            "actual nonfinite PCM cannot be published as a playable bank preview");
        bad.mediaFile = changedBankSource; event.media = { good, bad };
        auto changed = operation("changed-bank");
        std::thread changedWorker([&] { preview = backend.preparePreview(*bankState, event, {}, changed); });
        await([&] { return invocations(changedBankSource) == 1; });
        require(bank.appendData("x", 1) && gate(changedBankSource).replaceWithText("finish"), "change actual bank after decoder admission");
        changedWorker.join();
        require(!preview.isValid() && changed->report().failure && changed->report().failure->reason == Policy::Reason::ioFailure
            && changed->report().process && changed->report().process->jobCleanupConfirmed,
            "successful decoder cannot publish a composite under a stale physical bank index");
        exited(changedBankSource);
    }
    for (const auto& input : { success, failure, cancel, waiting, quota, deadlineFirst, deadlineSecond,
                              backendFailure, alternative, backendCancel, changedBankSource })
    {
        exited(input);
        const auto wave = VgmstreamDecodeService::getDecodedWaveFile(input);
        juce::InterProcessLock lock(lockName(wave)); require(lock.enter(5000), "fixture cache producer released");
        require(wave.getParentDirectory() == VgmstreamDecodeService::getDecodeCacheDirectory() && wave.deleteFile()
            && DecodedWaveCache::sealFile(wave).deleteFile()
            && wave.getSiblingFile(wave.getFileName() + ".decoding").deleteFile(), "remove only this test's exact cache files after all readers release");
    }
    require(_wputenv_s(L"SINGLE_EDITOR_VGMSTREAM_PATH", previous.toWideCharPointer()) == 0, "restore only this test process's environment");
    require(root.getParentDirectory() == parent && root.getFileName().startsWith("SingleEditor-BankDecode-")
        && root.deleteRecursively(), "remove only private bank decoder fixture");
    std::cout << "BankDecodeOperationTests passed (real composite failure/cancel/fuse, no fallback/partial/nonfinite/stale-bank publication, cache checkpoints, finite quota expansion, shared real deadline)\n";
#endif
}

void runBankPreviewRequestTests()
{
#if JUCE_WINDOWS
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-BankRequest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private frozen request fixture");
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto oldProducer = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_VGMSTREAM_PATH", {});
    require(_wputenv_s(L"SINGLE_EDITOR_VGMSTREAM_PATH", executable.getFullPathName().toWideCharPointer()) == 0, "set only this request test's decoder");
    const auto bank = root.getChildFile("request.bnk");
    juce::MemoryOutputStream bytes;
    bytes.write("BKHD", 4); bytes.writeInt(8); bytes.writeInt(128); bytes.writeInt(7);
    require(bank.replaceWithData(bytes.getData(), bytes.getDataSize()), "write real request bank");
    auto parsed = BnkParser::parseFile(bank, root);
    require(parsed && parsed->isSourceCurrent(), "parse real bank physical identity");
    auto policy = std::make_shared<Policy>();
    std::vector<std::pair<juce::File, juce::File>> inputs;
    const auto make = [&](const char* name, const char* mode, bool finished = true)
    {
        const auto file = root.getChildFile(name);
        require(file.replaceWithText(juce::String("packed-fixture:") + mode), "create actual request decoder source");
        if (finished) require(gate(file).replaceWithText("finish"), "release actual request decoder source");
        inputs.emplace_back(file, VgmstreamDecodeService::getDecodedWaveFile(file));
        return file;
    };
    const auto op = [&](Policy::Intent intent = Policy::Intent::initial, std::optional<Policy::Key> key = {})
    { return std::make_shared<OptionalMediaOperation>(policy, intent, key, Policy::Module::bank); };
    const auto raw = [&](const juce::File& file, uint32_t id)
    {
        BnkDocument::PlayableMediaDescriptor media;
        media.mediaId = id; media.mediaFile = file;
        return BnkPreviewRequest::forMedia(parsed, media, BnkMediaResolveMode::decodePackedMedia);
    };
    const auto good = make("good.wem", "success");
    {
        auto request = raw(good, 1); auto work = op(); auto preview = request->prepare(work);
        require(preview.isValid() && preview.state->info.numSamples == 144000 && work->report().key
            && !work->report().terminal && preview.state->audioBuffer->getSample(0, 96032) == 0.25f,
            "request derives actual bank/media/dependency identity and returns full PCM without owner admission");
        preview = {}; work->succeed();
    }
    const auto changed = make("changed.wem", "late-exit");
    {
        auto request = raw(changed, 2); auto first = op();
        require(!request->prepare(first).isValid() && first->report().failure && invocations(changed) == 1,
            "actual frozen request reaches one decoder failure");
        require(changed.appendText(" changed"), "change actual dependency after its failed attempt");
        auto retry = op(Policy::Intent::retryOnce, first->report().key);
        require(!request->prepare(retry).isValid() && retry->report().denied == Policy::Denied::invalidKey && invocations(changed) == 1,
            "reviewed retry cannot authorize a physically changed media dependency");
    }
    const auto randomA = make("random-a.wem", "late-exit"), randomB = make("random-b.wem", "late-exit");
    BnkDocument::EventDescriptor event;
    event.id = 42; event.playbackHint = BnkDocument::PlaybackHint::random;
    BnkDocument::EventDescriptor::MediaReference a, b;
    a.mediaId = 42; a.mediaFile = randomA; b.mediaId = 43; b.mediaFile = randomB;
    event.media = { a, b }; parsed->events.push_back(event);
    {
        auto request = BnkPreviewRequest::forEvent(parsed, 42, {}, BnkMediaResolveMode::decodePackedMedia);
        auto first = op(); require(!request->prepare(first).isValid() && first->report().failure, "actual random member fails once");
        const auto chosen = invocations(randomA) == 1 ? randomA : randomB;
        const auto other = chosen == randomA ? randomB : randomA;
        auto retry = op(Policy::Intent::retryOnce, first->report().key);
        require(!request->prepare(retry).isValid() && retry->policyView().circuitOpen
            && invocations(chosen) == 2 && invocations(other) == 0,
            "same immutable random request retries the same actual member and opens its fuse");
        require(first->report().key->sourceFingerprint == retry->report().key->sourceFingerprint,
            "random retry preserves the real selection identity");
    }
    const auto singleRandom = make("single-random.wem", "late-exit");
    event.id = 44; a.mediaId = 44; a.mediaFile = singleRandom; event.media = { a }; parsed->events.push_back(event);
    {
        auto first = op(); auto request = BnkPreviewRequest::forEvent(parsed, 44, {}, BnkMediaResolveMode::decodePackedMedia);
        require(!request->prepare(first).isValid() && first->report().failure, "single random choice fails once");
        BnkPlaybackContext changedContext; changedContext.stateValueToken = "different-state";
        auto different = BnkPreviewRequest::forEvent(parsed, 44, changedContext, BnkMediaResolveMode::decodePackedMedia);
        auto staleContext = op(Policy::Intent::retryOnce, first->report().key);
        require(!different->prepare(staleContext).isValid() && staleContext->report().denied == Policy::Denied::invalidKey
            && invocations(singleRandom) == 1, "reviewed retry cannot cross actual playback context even for the same single random member");
        auto retry = op(Policy::Intent::retryOnce, first->report().key);
        auto recreated = BnkPreviewRequest::forEvent(parsed, 44, {}, BnkMediaResolveMode::decodePackedMedia);
        require(!recreated->prepare(retry).isValid() && retry->policyView().circuitOpen && invocations(singleRandom) == 2,
            "new random seed is not a new source identity or a fuse bypass when actual selection is unchanged");
        auto blocked = op();
        require(!recreated->prepare(blocked).isValid() && blocked->report().denied == Policy::Denied::circuitOpen && invocations(singleRandom) == 2,
            "recreated logical request cannot automatically rerun a fused actual selection");
    }
    const auto waiting = make("guarded.wem", "hang", false);
    {
        auto request = raw(waiting, 5); auto work = op(); BnkPreparedPlayback preview;
        std::thread worker([&] { preview = request->prepare(work); });
        await([&] { return invocations(waiting) == 1; });
        require(!bank.appendData("x", 1) && !waiting.appendText("changed"),
            "actual request leases deny bank and selected input writes while the real child owns them");
        const auto unrelated = root.getChildFile("unselected.txt");
        require(unrelated.replaceWithText("allowed"), "request does not lock unrelated content in the same directory");
        work->requestCancel(); worker.join();
        require(!preview.isValid() && work->report().cancelled && work->report().process && work->report().process->jobCleanupConfirmed,
            "cancelled frozen request drains its actual child without candidate publication");
        exited(waiting);
        require(waiting.appendText("after release") && bank.appendData("x", 1), "worker completion releases every source lease");
    }
    for (const auto& [file, cache] : inputs)
    {
        exited(file);
        juce::InterProcessLock lock(lockName(cache)); require(lock.enter(5000), "owned frozen request cache producer released");
        require(cache.getParentDirectory() == VgmstreamDecodeService::getDecodeCacheDirectory() && cache.deleteFile()
            && DecodedWaveCache::sealFile(cache).deleteFile() && cache.getSiblingFile(cache.getFileName() + ".decoding").deleteFile(),
            "clean only exact test caches captured before source changes");
    }
    require(_wputenv_s(L"SINGLE_EDITOR_VGMSTREAM_PATH", oldProducer.toWideCharPointer()) == 0, "restore this request test's process environment");
    require(root.getParentDirectory() == parent && root.getFileName().startsWith("SingleEditor-BankRequest-") && root.deleteRecursively(),
            "remove only private frozen request fixture");
    std::cout << "BankPreviewRequestTests passed (actual dependency-bound retry, fixed random member, seed-independent fuse, physical leases, real child cancel/drain)\n";
#endif
}
