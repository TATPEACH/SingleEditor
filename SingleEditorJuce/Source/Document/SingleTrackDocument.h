#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "Audio/AmbisonicMonitorState.h"
#include "Audio/ChannelLayout.h"
#include "Document/MediaIdentity.h"
#include "Audio/ReaderAdmission.h"
#include "Audio/SourceFileLease.h"

namespace singleeditor
{
namespace media { class OptionalMediaOperation; }
namespace recovery { class RecoveryOwnerLifecycle; }
class AmbisonicBinauralFilterBank;
class SingleTrackDocument final
{
    class RenderedAudioStorage;
public:
    enum class WemOpenStrategy
    {
        notWem = 0,
        directWaveReader,
        vgmstreamFallback
    };

    struct EnvelopePoint
    {
        double timeSeconds = 0.0;
        float gain = 1.0f;
    };

    struct OfflineAmplitudeEdit
    {
        enum class Kind
        {
            gain,
            fadeIn,
            fadeOut
        };

        Kind kind = Kind::gain;
        juce::Range<int64_t> sampleRange;
        float linearGain = 1.0f;
    };

    struct OfflineEditPipeline
    {
        juce::Range<int64_t> deletionRange;
        std::vector<OfflineAmplitudeEdit> amplitudeEdits;
    };

    struct FileInfo
    {
        juce::File file;
        juce::String formatName;
        juce::StringPairArray metadata;
        double durationSeconds = 0.0;
        double sampleRate = 0.0;
        int numChannels = 0;
        int bitsPerSample = 0;
        int64_t numSamples = 0;
        ChannelLayoutState channelLayout;
        // Set only when adopting a verified recovery candidate with independent
        // audio ownership. Its historical locator need not exist any longer.
        bool recoveryBacked = false;
        // A real reader/buffer has been admitted and is owned by State. Moving
        // its original locator cannot erase a live document or its dirty state.
        // This is never source identity or permission to overwrite that path.
        bool sourceAdmitted = false;

        bool isValid() const noexcept { return numSamples > 0 && (sourceAdmitted || recoveryBacked || file.existsAsFile()); }
    };

    struct State
    {
        struct PlaybackAvailability
        {
            enum class Access
            {
                unavailable = 0,
                contiguousPrefix,
                randomAccess
            };

            PlaybackAvailability()
                : sharedState(std::make_shared<SharedState>())
            {
            }

            static PlaybackAvailability contiguousPrefix(int64_t readySamples) noexcept
            {
                PlaybackAvailability availability;
                availability.sharedState->readyEndSample.store(juce::jmax<int64_t>(0, readySamples),
                                                               std::memory_order_relaxed);
                availability.sharedState->canAdvance.store(true, std::memory_order_relaxed);
                availability.sharedState->access.store(Access::contiguousPrefix, std::memory_order_release);
                return availability;
            }

            static PlaybackAvailability randomAccess() noexcept
            {
                PlaybackAvailability availability;
                availability.sharedState->access.store(Access::randomAccess, std::memory_order_release);
                return availability;
            }

            Access getAccess() const noexcept
            {
                return sharedState->access.load(std::memory_order_acquire);
            }

            bool isRandomAccess() const noexcept
            {
                return getAccess() == Access::randomAccess;
            }

            void advanceContiguousPrefix(int64_t readySamples) const noexcept
            {
                if (!canAdvanceContiguousPrefix())
                    return;

                const auto nextReadyEnd = juce::jmax<int64_t>(0, readySamples);
                auto currentReadyEnd = sharedState->readyEndSample.load(std::memory_order_relaxed);
                while (currentReadyEnd < nextReadyEnd
                       && !sharedState->readyEndSample.compare_exchange_weak(currentReadyEnd,
                                                                             nextReadyEnd,
                                                                             std::memory_order_release,
                                                                             std::memory_order_relaxed))
                {
                }
            }

            void markRandomAccess() const noexcept
            {
                sharedState->access.store(Access::randomAccess, std::memory_order_release);
                sharedState->canAdvance.store(false, std::memory_order_release);
            }

            bool canAdvanceContiguousPrefix() const noexcept
            {
                return getAccess() == Access::contiguousPrefix && sharedState->canAdvance.load(std::memory_order_acquire);
            }

            // Terminal producer failure/cancel preserves already verified audio
            // while telling background consumers that no more samples will arrive.
            void sealContiguousPrefix() const noexcept
            {
                sharedState->canAdvance.store(false, std::memory_order_release);
            }

            void markUnavailable() const noexcept
            {
                sharedState->access.store(Access::unavailable, std::memory_order_release);
                sharedState->canAdvance.store(false, std::memory_order_release);
            }

            juce::Range<int64_t> getPlayableSampleRange(int64_t totalSamples) const noexcept
            {
                const auto fileEnd = juce::jmax<int64_t>(0, totalSamples);
                const auto currentAccess = getAccess();
                if (currentAccess == Access::randomAccess)
                    return { 0, fileEnd };
                if (currentAccess == Access::contiguousPrefix)
                {
                    const auto readyEnd = sharedState->readyEndSample.load(std::memory_order_acquire);
                    return { 0, juce::jlimit<int64_t>(0, fileEnd, readyEnd) };
                }
                return {};
            }

            bool canPlaySample(int64_t sample, int64_t totalSamples) const noexcept
            {
                return getPlayableSampleRange(totalSamples).contains(sample);
            }

        private:
            struct SharedState
            {
                std::atomic<Access> access { Access::unavailable };
                std::atomic<int64_t> readyEndSample { 0 };
                std::atomic<bool> canAdvance { false };
            };

            std::shared_ptr<SharedState> sharedState;
        };

        enum class AudioBackingStore
        {
            empty = 0,
            previewOnly,
            inMemoryBuffer,
            memoryMappedFile,
            streamingReader,
            // Isolated verified recovery reader; never reopened by source path.
            // Prefix readiness may grow after the immutable State is published.
            recoveryStream
        };

        FileInfo info;
        std::shared_ptr<juce::AudioBuffer<float>> audioBuffer = std::make_shared<juce::AudioBuffer<float>>();
        std::shared_ptr<juce::MemoryMappedAudioFormatReader> memoryMappedReader;
        std::shared_ptr<juce::AudioFormatReader> streamingReader;
        juce::File readerSourceFile;
        media::SourceFileWitness::Ptr sourceFileWitness;
        // True only for audio admitted from info.file (including its decoded
        // reader). History copies preserve this fact; rendered PCM clears it.
        bool matchesSourceFileAudio = false;
        AudioBackingStore audioBackingStore = AudioBackingStore::empty;
        PlaybackAvailability playbackAvailability;
        AmbisonicOrientation ambisonicOrientation;
        AmbisonicMonitorMode ambisonicMonitorMode = AmbisonicMonitorMode::speakerProjection;
        std::shared_ptr<const AmbisonicBinauralFilterBank> ambisonicBinauralFilterBank;
        juce::String ambisonicBinauralStatus;
        std::vector<EnvelopePoint> envelopePoints;
        bool hasTimelineEdits = false;
        juce::StringArray safeTimelineMetadataKeys;
        juce::StringArray transformedMetadataKeys;
        int64_t audioRevision = 0;
        int64_t envelopeRevision = 0;
        int64_t changeGeneration = 0;

        bool hasPlaybackSource() const noexcept
        {
            return (audioBuffer != nullptr && audioBuffer->getNumSamples() > 0)
                || memoryMappedReader != nullptr
                || streamingReader != nullptr;
        }

        bool isPreviewOnly() const noexcept
        {
            return audioBackingStore == AudioBackingStore::previewOnly;
        }

        juce::Range<int64_t> getPlayableSampleRange() const noexcept
        {
            return playbackAvailability.getPlayableSampleRange(info.numSamples);
        }

        bool canPlaySample(int64_t sample) const noexcept
        {
            return playbackAvailability.canPlaySample(sample, info.numSamples);
        }
    };

    using PlaybackAvailability = State::PlaybackAvailability;

    struct PreparedFileLoad
    {
        FileInfo info;
        std::shared_ptr<juce::MemoryMappedAudioFormatReader> memoryMappedReader;
        std::unique_ptr<juce::AudioFormatReader> ownedReader;
        juce::File readerSourceFile;
        media::SourceFileWitness::Ptr sourceFileWitness;
        juce::String errorMessage;
        media::ReaderFailure admissionFailure = media::ReaderFailure::none;
        PlaybackAvailability playbackAvailability = PlaybackAvailability::randomAccess();

        juce::AudioFormatReader* getReader() const noexcept
        {
            if (memoryMappedReader != nullptr)
                return memoryMappedReader.get();

            return ownedReader.get();
        }

        bool hasReader() const noexcept
        {
            return getReader() != nullptr;
        }
    };

    using StatePtr = std::shared_ptr<const State>;

    // Published transition facts, not a command or Preview state. Unsupported
    // or ambiguous transitions have no descriptor and require a checkpoint.
    struct CanonicalMutation final
    {
        enum class Kind { envelopeReplace, undo, redo };
        Kind kind = Kind::envelopeReplace;
        media::DocumentIdentity before, after;
        juce::Uuid sourceBinding { nullptr };
        bool pushesUndo = false;
    };

    // Captured only by an explicitly attached post-Core consumer. References
    // preserve immutable state/Undo ownership without copying or reading audio.
    struct CommittedStateSnapshot final
    {
        media::IdentitySnapshot identity;
        StatePtr state;
        std::vector<StatePtr> undo;
        std::vector<StatePtr> redo;
        std::optional<CanonicalMutation> mutation;
        // Original source/Save publication facts, independent of edited State
        // dimensions. Copying this pointer performs no path or audio I/O.
        media::SourceFileWitness::Ptr sourceVersion;
    };
    std::optional<CommittedStateSnapshot> captureCommittedState(const media::DocumentIdentity& expected) const;
    // Adoption of an already validated immutable checkpoint with memory buffers
    // or explicit isolated recoveryStream readers. Current audio needs a real
    // prefix; deferred history may still be unprepared. The caller separately
    // proves first-waveform readiness before product/playlist attachment.
    // Creates an isolated dirty document; never replaces a live document, reads
    // the historical path or inherits its right to checkpoint/overwrite it.
    static std::unique_ptr<SingleTrackDocument> createRecoveredCopy(const CommittedStateSnapshot&, juce::String& error);
    bool requiresRecoverySaveAs() const;
    std::optional<media::DocumentIdentity> getRecoveryOrigin() const;

    class EqualLengthAudioMutationTransaction final
    {
    public:
        bool isValid() const noexcept;
        int getNumChannels() const noexcept;
        int64_t getNumSamples() const noexcept;
        double getSampleRate() const noexcept;
        StatePtr getSourceState() const noexcept;
        bool readSourceBlock(int64_t startSample,
                             int numSamples,
                             juce::AudioBuffer<float>& destination) const;
        bool readRenderedBlock(int64_t startSample,
                               int numSamples,
                               juce::AudioBuffer<float>& destination) const;
        bool writeRenderedBlock(int64_t startSample,
                                const juce::AudioBuffer<float>& source,
                                int sourceStartSample,
                                int numSamples);

    private:
        friend class SingleTrackDocument;

        EqualLengthAudioMutationTransaction(StatePtr expectedState,
                                            std::shared_ptr<juce::AudioBuffer<float>> sourceAudio,
                                            std::shared_ptr<juce::AudioBuffer<float>> stagingAudio);
        EqualLengthAudioMutationTransaction(StatePtr expectedState,
                                            std::shared_ptr<juce::AudioFormatReader> sourceReader,
                                            std::shared_ptr<media::SourceFileLease> sourceLease,
                                            std::shared_ptr<RenderedAudioStorage> stagingFile);

        StatePtr expectedState;
        media::DocumentIdentity expectedIdentity;
        media::SourceIdentity expectedSourceIdentity;
        std::shared_ptr<juce::AudioBuffer<float>> sourceAudio;
        std::shared_ptr<juce::AudioBuffer<float>> stagingAudio;
        std::shared_ptr<juce::AudioFormatReader> sourceReader;
        std::shared_ptr<media::SourceFileLease> sourceLease;
        std::shared_ptr<RenderedAudioStorage> stagingFile;
        int64_t nextWriteSample = 0;
    };

    enum class SaveIntent
    {
        checkpoint = 0,
        saveAs,
        exportCopy
    };

    struct SaveEncodingOptions
    {
        int mp3BitrateKbps = 192;
        int oggQualityIndex = 6;
        int flacCompressionLevel = 5;
    };

    struct SourceFileIdentity
    {
        juce::File file;
        int64_t size = -1;
        int64_t lastModificationMilliseconds = 0;
        bool valid = false;

        static SourceFileIdentity capture(const juce::File& sourceFile);
        bool matchesCurrentFile() const;
    };

    enum class SaveFailureReason
    {
        none = 0,
        invalidSnapshot,
        staleSource,
        metadataRejected,
        unsupportedFormat,
        writeFailed,
        validationFailed,
        replaceFailed,
        durabilityFailed
    };

    struct SaveMetadataReport
    {
        juce::StringArray preservedKeys;
        juce::StringArray transformedKeys;
        juce::StringArray omittedKeys;
        juce::StringArray rejectedKeys;
        juce::String description;

        bool canProceed() const noexcept { return rejectedKeys.isEmpty(); }
    };

    struct SaveSnapshot
    {
        StatePtr state;
        juce::File targetFile;
        int64_t changeGeneration = 0;
        SaveIntent intent = SaveIntent::checkpoint;
        SaveEncodingOptions encodingOptions;
        SaveMetadataReport metadataReport;
        SourceFileIdentity sourceIdentity;
        media::DocumentIdentity documentIdentity;
        bool recoveryRequiresSaveAs = false;
        juce::Uuid sourceBinding { nullptr };
        uint64_t requestSerial = 0;
        media::SourceFileWitness::Ptr sourceVersion;

        bool isValid() const noexcept
        {
            return state != nullptr && state->info.isValid() && targetFile != juce::File()
                && (!state->info.recoveryBacked || (state->hasPlaybackSource()
                    && state->getPlayableSampleRange().getEnd() == state->info.numSamples))
                && !(recoveryRequiresSaveAs && intent == SaveIntent::checkpoint);
        }
    };

    // Minted only by the real save writer after publication/flush. Weak state
    // ownership does not retain a document's audio after a result is kept.
    class SavePublication final
    {
    public:
        const media::SourceFileWitness::Ptr& targetVersion() const noexcept { return file; }
        const media::DocumentIdentity& documentIdentity() const noexcept { return identity; }
        const juce::Uuid& previousSourceBinding() const noexcept { return sourceBinding; }
        SaveIntent saveIntent() const noexcept { return intent; }
        const media::SourceFileWitness::Ptr& replacedSourceVersion() const noexcept { return replacedSource; }
    private:
        friend class SingleTrackDocument;
        friend class recovery::RecoveryOwnerLifecycle;
        SavePublication(const SaveSnapshot&, media::SourceFileWitness::Ptr, media::SourceFileWitness::Ptr);
        bool matches(const SaveSnapshot&) const;
        std::weak_ptr<const State> state;
        media::SourceFileWitness::Ptr file;
        media::SourceFileWitness::Ptr replacedSource;
        std::weak_ptr<const media::SourceFileWitness> requestedSource;
        media::DocumentIdentity identity;
        juce::Uuid sourceBinding { nullptr };
        SaveIntent intent;
        uint64_t requestSerial = 0;
    };

    struct SaveWriteResult
    {
        bool succeeded = false;
        juce::File targetFile;
        int64_t changeGeneration = 0;
        juce::String errorMessage;
        juce::String encodingDescription;
        juce::String metadataDescription;
        SaveFailureReason failureReason = SaveFailureReason::none;
        std::shared_ptr<const SavePublication> publication;
    };

    // Immutable, private-issued workspace image. Unlike a raw State this carries
    // full Undo/Redo and the real source/save restrictions. It owns shared audio,
    // not a second decoder or PCM copy; it is not serialized as session history.
    class WorkspaceSnapshot final
    {
    public:
        using Ptr = std::shared_ptr<const WorkspaceSnapshot>;
        const StatePtr& state() const noexcept { return contents.state; }
        bool hasUnsavedChanges() const noexcept;
        // Exact immutable image comparison for a UI decision. Adoption creates
        // a fresh identity; equal version numbers never imply temporal order.
        bool sameContents(const WorkspaceSnapshot&) const noexcept;
    private:
        friend class SingleTrackDocument;
        WorkspaceSnapshot() = default;
        CommittedStateSnapshot contents;
        juce::File target;
        SourceFileIdentity source;
        media::SourceFileWitness::Ptr sourceVersion;
        bool requiresSaveAs = false;
        std::optional<media::DocumentIdentity> origin;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void documentChanged(StatePtr) {}
        virtual void documentIdentityChanged(const media::IdentityEvent&) {}
    };

    SingleTrackDocument();
    media::IdentitySnapshot getMediaIdentity() const;
    // Explicit owner-thread workspace retention/adoption, no filesystem/PCM I/O.
    // Adoption is a new live document identity, preserving state equivalence,
    // clean/dirty relationships, full history and recovery/source restrictions.
    // Old async Save receipts cannot cross this activation boundary.
    WorkspaceSnapshot::Ptr captureWorkspaceSnapshot() const;
    bool adoptWorkspaceSnapshot(const WorkspaceSnapshot&, juce::String& error);

    static WemOpenStrategy getWemOpenStrategy(const juce::File& file);
    static std::unique_ptr<juce::AudioFormatReader> createReaderForFile(const juce::File& file);
    static std::unique_ptr<juce::AudioFormatReader> createReaderForRetainedSource(const media::SourceFileWitness::Ptr&);
    static PreparedFileLoad prepareAudioFileForOpen(const juce::File& file,
        const std::shared_ptr<media::OptionalMediaOperation>& optionalOperation = {});
    static FileInfo buildFileInfoForReader(const juce::File& file, const juce::AudioFormatReader& reader);
    static FileInfo probeFileInfo(const juce::File& file);
    static std::shared_ptr<State> createPreviewState(const FileInfo& info);
    static std::shared_ptr<State> createStateFromPreparedFile(PreparedFileLoad prepared);
    static std::shared_ptr<State> createStateFromOwnedReader(const juce::File& file,
                                                             std::unique_ptr<juce::AudioFormatReader> reader);
    static std::shared_ptr<State> createStateFromReader(const juce::File& file, juce::AudioFormatReader& reader);
    static std::shared_ptr<State> createStateFromFile(const juce::File& file);
    bool loadFile(const juce::File& file);
    bool adoptLoadedState(std::shared_ptr<State> state);
    bool adoptPreviewState(const FileInfo& info);
    void clear();

    StatePtr getState() const noexcept;
    FileInfo getFileInfo() const;
    bool hasAudio() const noexcept;
    bool hasUnsavedChanges() const noexcept;
    double getDurationSeconds() const noexcept;
    int64_t getNumSamples() const noexcept;
    double getSampleRate() const noexcept;
    juce::Range<int64_t> getPlayableSampleRange() const noexcept;
    juce::Range<double> getPlayableTimeRange() const noexcept;
    bool setChannelLayoutPreset(const juce::String& presetId);
    AmbisonicOrientation getAmbisonicOrientation() const noexcept;
    bool setAmbisonicOrientation(AmbisonicOrientation orientation);
    AmbisonicMonitorMode getAmbisonicMonitorMode() const noexcept;
    bool setAmbisonicMonitorMode(AmbisonicMonitorMode mode);
    bool applyPreparedBinauralMonitor(StatePtr expectedState,
                                     std::shared_ptr<const AmbisonicBinauralFilterBank> bank,
                                     const juce::String& errorMessage);
    juce::String getAmbisonicBinauralStatus() const;

    std::vector<EnvelopePoint> getEnvelopePoints() const;
    float getEnvelopeGainAt(double timeSeconds) const noexcept;
    static float evaluateEnvelope(const std::vector<EnvelopePoint>& points, double timeSeconds) noexcept;
    static bool isEnvelopeFlatUnity(const std::vector<EnvelopePoint>& points) noexcept;
    void resetEnvelope();
    int insertEnvelopePoint(double timeSeconds);
    void beginEnvelopeGesture();
    void endEnvelopeGesture() noexcept;
    bool moveEnvelopePoint(int index, double timeSeconds, float gain);
    bool removeEnvelopePoint(int index);

    bool applySelectionGain(const juce::Range<double>& selectionSeconds, float linearGain);
    bool applyFadeIn(const juce::Range<double>& selectionSeconds);
    bool applyFadeOut(const juce::Range<double>& selectionSeconds);
    bool deleteSelection(const juce::Range<double>& selectionSeconds);
    bool applyOfflineEditPipeline(const OfflineEditPipeline& pipeline);
    std::unique_ptr<EqualLengthAudioMutationTransaction> beginEqualLengthAudioMutation(
        StatePtr expectedState);
    bool commitEqualLengthAudioMutation(
        std::unique_ptr<EqualLengthAudioMutationTransaction> transaction);
    bool canSaveToFile(const juce::File& targetFile) const;
    bool canSaveOverSourceFile() const;
    juce::String getDefaultSaveExtension() const;
    SaveSnapshot createSaveSnapshot(const juce::File& targetFile = {},
                                    SaveIntent intent = SaveIntent::checkpoint,
                                    SaveEncodingOptions encodingOptions = {}) const;
    static SaveWriteResult writeSaveSnapshot(const SaveSnapshot& snapshot);
    // Rejects fabricated/stale results and a replaced/changed saved file. A
    // successful old-generation save may still leave newer edits dirty.
    bool markSaveSnapshotCommitted(const SaveSnapshot& snapshot, const SaveWriteResult&);
    bool saveAsToFile(const juce::File& targetFile);
    bool saveToFile(const juce::File& targetFile);
    bool saveOverSourceFile();

    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    static constexpr int maxUndoSteps = 10;

    static std::vector<EnvelopePoint> makeDefaultEnvelope(double durationSeconds);
    static void sanitiseEnvelope(std::vector<EnvelopePoint>& points, double durationSeconds);
    static juce::Range<int64_t> secondsToSampleRange(const State& state, const juce::Range<double>& seconds) noexcept;
    static std::shared_ptr<juce::AudioBuffer<float>> copyBuffer(const juce::AudioBuffer<float>& source);
    static bool materialiseAudioBufferIfNeeded(State& state);
    static bool writeStateAudioToFile(const State& state,
                                      const juce::File& targetFile,
                                      const SaveEncodingOptions& encodingOptions,
                                      const juce::StringArray& omittedMetadataKeys);
    static StatePtr makeEmptyState();
    static void registerFormats(juce::AudioFormatManager& manager);
    static bool canWriteExtension(const juce::String& extension) noexcept;
    static std::unique_ptr<juce::AudioFormat> createWriterFormatForExtension(const juce::String& extension);

    void markCurrentStateClean(StatePtr savedState = {});
    void pushUndoState(StatePtr state);
    void commitState(std::shared_ptr<State> newState, bool pushUndo,
                     std::optional<CanonicalMutation::Kind> = std::nullopt);
    void notifyDocumentChanged(StatePtr state);
    void notifyIdentityChanged(std::optional<media::IdentityEventKind> forcedKind = std::nullopt);
    int64_t nextAudioRevisionNumber() noexcept;
    int64_t nextEnvelopeRevisionNumber() noexcept;
    int64_t nextChangeGenerationNumber() noexcept;

    juce::ListenerList<Listener> listeners;
    juce::CriticalSection stateLock;
    StatePtr currentState;
    juce::File documentTargetFile;
    SourceFileIdentity documentSourceIdentity;
    const juce::Uuid runtimeIdentity;
    juce::Uuid lineageIdentity { nullptr };
    juce::Uuid sourceBindingIdentity { nullptr };
    media::IdentitySnapshot lastNotifiedIdentity;
    uint64_t identityEventSequence = 0;
    std::optional<CanonicalMutation> lastCanonicalMutation;
    int64_t cleanChangeGeneration = 0;
    media::SourceFileWitness::Ptr documentSourceVersion;
    mutable uint64_t nextSaveRequestSerial = 0; // stateLock; only explicit snapshot creation.
    uint64_t appliedSaveRequestSerial = 0;
    bool recoveryRequiresSaveAs = false;
    std::optional<media::DocumentIdentity> recoveryOrigin;
    std::vector<StatePtr> undoStack;
    std::vector<StatePtr> redoStack;
    StatePtr envelopeGestureUndoState;
    bool envelopeGestureActive = false;
    bool envelopeGestureDidMutate = false;
    std::atomic<int64_t> nextAudioRevision { 1 };
    std::atomic<int64_t> nextEnvelopeRevision { 1 };
    std::atomic<int64_t> nextChangeGeneration { 1 };
};
} // namespace singleeditor
