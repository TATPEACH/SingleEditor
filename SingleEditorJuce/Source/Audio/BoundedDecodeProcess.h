#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor::media
{
// A resource boundary for an explicitly requested optional decoder, not an OS
// security sandbox. Call on a worker. No service, thread or I/O before run().
class BoundedDecodeProcess final
{
public:
    enum class Failure { none, invalidRequest, unsupportedPlatform, launch,
                         containment, cancelled, deadline, outputLimit,
                         invalidOutput, decoderExit, cleanup };
    struct Limits final
    {
        int wallTimeMs = 30000;
        uint64_t memoryBytes = 2ULL * 1024 * 1024 * 1024;
        uint64_t outputBytes = 4ULL * 1024 * 1024 * 1024;
        uint32_t processes = 1;
    };
    struct Result final
    {
        Failure failure = Failure::none;
        uint32_t exitCode = 0;
        uint32_t nativeError = 0;
        uint64_t observedOutputBytes = 0;
        uint32_t elapsedMilliseconds = 0;
        bool jobCleanupConfirmed = false;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        std::uint64_t observedResourceId = 0;
#endif
        explicit operator bool() const noexcept { return failure == Failure::none; }
    };
    // Arguments exclude argv[0]. Staging is owned by the caller. The callback
    // may inspect a growing file; failure terminates the complete owned Job.
    // File-size monitoring can overshoot by one poll; callers must validate
    // dimensions and completeness before publishing any completed cache entry.
    static Result run(const juce::File& executable, const juce::StringArray& arguments,
                      const juce::File& staging, const Limits& limits,
                      const std::atomic<bool>& cancel,
                      const std::function<Failure()>& inspect = {}
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                      , std::uint64_t observedParentResource = 0,
                      OptionalLifecycleOwner observedOwner = OptionalLifecycleOwner::optionalMedia
#endif
                      );
    static const char* describe(Failure) noexcept;
};
}
