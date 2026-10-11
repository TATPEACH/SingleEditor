#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
class WaveformModel final : public SingleTrackDocument::Listener
{
public:
    struct PersistentCacheStats
    {
        int fileCount = 0;
        int64_t totalBytes = 0;
    };

    struct PeakBin
    {
        float minValue = 0.0f;
        float maxValue = 0.0f;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
    };

    struct PeakCache
    {
        struct PeakLodLevel
        {
            int binCount = 0;
            std::vector<std::vector<PeakBin>> channelBins;
            std::vector<PeakBin> mixedBins;
        };

        SingleTrackDocument::StatePtr state;
        int64_t audioRevision = 0;
        double durationSeconds = 0.0;
        double sampleRate = 0.0;
        int64_t totalSamples = 0;
        juce::Range<int64_t> loadedSampleRange;
        int channelCount = 0;
        int binCount = 0;
        bool isPreview = false;
        std::vector<std::vector<PeakBin>> channelBins;
        std::vector<PeakBin> mixedBins;
        std::vector<PeakLodLevel> lodLevels;
    };

    struct PeakLevelView
    {
        int binCount = 0;
        const std::vector<std::vector<PeakBin>>* channelBins = nullptr;
        const std::vector<PeakBin>* mixedBins = nullptr;
    };

    struct PersistentPeakCacheLookup
    {
        SingleTrackDocument::FileInfo info;
        std::shared_ptr<PeakCache> cache;
    };

    struct SpectrogramCache
    {
        SingleTrackDocument::StatePtr state;
        int64_t audioRevision = 0;
        double durationSeconds = 0.0;
        double sampleRate = 0.0;
        int width = 0;
        int height = 0;
        bool isPreview = false;
        juce::Image image;
    };

    explicit WaveformModel(SingleTrackDocument& documentToUse);
    ~WaveformModel() override;

    bool hasWaveform() const noexcept;
    double getLengthInSeconds() const noexcept;
    int getChannelCount() const noexcept;
    SingleTrackDocument::StatePtr getState() const noexcept;
    juce::Range<float> getChannelMinMax(int channel, int64_t startSample, int64_t endSample) const noexcept;
    float getEnvelopeGainAt(double timeSeconds) const noexcept;
    std::shared_ptr<const PeakCache> getPeakCache() const noexcept;
    std::shared_ptr<const SpectrogramCache> getSpectrogramCache() const noexcept;
    bool isPeakCacheReady() const noexcept;
    bool isSpectrogramCacheReady() const noexcept;
    enum class SpectrogramStatus { notRequested, preparing, ready, failed };
    SpectrogramStatus getSpectrogramStatus() const noexcept;
    void ensureSpectrogramReady(bool retryAfterFailure = false);
    void adoptPeakCache(std::shared_ptr<PeakCache> cache);
    void adoptSpectrogramCache(std::shared_ptr<SpectrogramCache> cache);
    void prepareIncomingCaches(std::shared_ptr<PeakCache> incomingPeakCache,
                               std::shared_ptr<SpectrogramCache> incomingSpectrogramCache = {});
    // MainComponent admits current-document persistence only after its real
    // CoreReady. Replacement/close cancels both pending and admitted writes.
    void dispatchPendingPeakCachePersistence();
    void cancelPendingPeakCachePersistence();
    static juce::File getPersistentCacheDirectory();
    static PersistentCacheStats getPersistentCacheStats();
    static void clearPersistentCache();
    static bool hasPersistentPeakCache(const juce::File& file);
    static bool hasPersistentStartupPreviewCache(const juce::File& file);
    static bool hasPersistentSpectrogramCache(const juce::File& file);
    static std::shared_ptr<PeakCache> buildPreviewPeakCacheFromReader(juce::AudioFormatReader& reader,
                                                                      int maxPreviewBins = 2048);
    static std::shared_ptr<PeakCache> buildLeadingPreviewPeakCacheFromReader(juce::AudioFormatReader& reader,
                                                                             int maxPreviewBins = 1024,
                                                                             double leadingSeconds = 8.0,
                                                                             int64_t readyEndSample = -1);
    static std::shared_ptr<PeakCache> buildLeadingPreviewPeakCacheForState(SingleTrackDocument::StatePtr state,
                                                                           int maxPreviewBins = 1024,
                                                                           double leadingSeconds = 8.0);
    static std::shared_ptr<PeakCache> buildPreviewPeakCacheForFile(const juce::File& file,
                                                                   int maxPreviewBins = 2048);
    static bool ensurePersistentPreviewCache(const juce::File& file,
                                             int maxPreviewBins = 2048);
    static bool ensurePersistentSpectrogramCache(const juce::File& file);
    static void persistStartupPreviewCache(const SingleTrackDocument::FileInfo& info,
                                           const PeakCache& cache);
    static std::optional<PersistentPeakCacheLookup> loadPersistentStartupPreviewForFile(const juce::File& file);
    static std::optional<PersistentPeakCacheLookup> lookupPersistentPeakCacheHeaderForFile(const juce::File& file);
    static std::optional<PersistentPeakCacheLookup> loadPersistentPeakCacheForFile(const juce::File& file);
    static std::shared_ptr<PeakCache> loadPersistentPeakCache(const SingleTrackDocument::FileInfo& info);
    static std::shared_ptr<PeakCache> loadPersistentPeakCache(SingleTrackDocument::StatePtr state);
    static std::shared_ptr<SpectrogramCache> loadPersistentSpectrogramCache(const SingleTrackDocument::FileInfo& info);
    static std::shared_ptr<SpectrogramCache> loadPersistentSpectrogramCache(SingleTrackDocument::StatePtr state);
    static PeakLevelView choosePeakLevel(const PeakCache& cache,
                                         juce::Range<double> visibleTimeRange,
                                         int targetPixelColumns) noexcept;

    static std::shared_ptr<PeakCache> buildPeakCacheSnapshot(SingleTrackDocument::StatePtr state,
                                                             int maxPreviewBins = 4096);
    // Explicit worker full-coverage build, with the normal display bins/LOD and
    // cancellation. No cache persistence, document listener or background job.
    static std::shared_ptr<PeakCache> buildCompletePeakCacheForState(SingleTrackDocument::StatePtr state,
                                                                    std::function<bool()> cancel = {});

    void addChangeListener(juce::ChangeListener* listener);
    void removeChangeListener(juce::ChangeListener* listener);

private:
    class PeakBuildJob;
    class PeakCachePersistenceJob;
    class SpectrogramBuildJob;
    friend class PeakBuildJob;
    friend class SpectrogramBuildJob;

    void documentChanged(SingleTrackDocument::StatePtr newState) override;
    void queuePeakCachePersistence(std::shared_ptr<const PeakCache> cache,
                                   SingleTrackDocument::StatePtr state, int64_t epoch);
    void schedulePeakCacheBuild(SingleTrackDocument::StatePtr state,
                                juce::Range<int64_t> seedLoadedSampleRange = {});
    void scheduleSpectrogramBuild(SingleTrackDocument::StatePtr state);
    void publishPeakCache(int64_t requestId,
                          std::shared_ptr<PeakCache> cache,
                          bool persistToDisk, int64_t persistenceRequestEpoch);
    void publishSpectrogramCache(int64_t requestId, SingleTrackDocument::StatePtr requestedState,
                                 std::shared_ptr<SpectrogramCache> cache);
    int64_t getLatestBuildRequestId() const noexcept;

    SingleTrackDocument& document;
    juce::ChangeBroadcaster broadcaster;
    mutable juce::CriticalSection cacheLock;
    std::shared_ptr<const PeakCache> peakCache;
    std::shared_ptr<const SpectrogramCache> spectrogramCache;
    SpectrogramStatus spectrogramStatus = SpectrogramStatus::notRequested; // guarded by cacheLock
    std::shared_ptr<PeakCache> pendingIncomingPeakCache;
    std::shared_ptr<SpectrogramCache> pendingIncomingSpectrogramCache;
    std::shared_ptr<const PeakCache> pendingPersistentPeakCache;
    SingleTrackDocument::StatePtr pendingPersistenceState;
    double persistenceDueAtMs = 0.0;
    std::atomic<bool> persistenceJobQueued { false };
    std::atomic<int64_t> persistenceEpoch { 0 };
    juce::ThreadPool buildPool { 1 };
    std::atomic<int64_t> latestBuildRequestId { 0 };
    std::atomic<int64_t> latestSpectrogramRequestId { 0 };
    std::atomic<int64_t> latestSpectrogramAudioRevisionRequested { -1 };
};
} // namespace singleeditor
