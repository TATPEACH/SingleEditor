#pragma once

// Native gate instrumentation only. Shipping/CLI builds have no counters,
// storage, callbacks or observer registration from this header.
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
#include <array>
#include <atomic>
#include <cstdint>
namespace singleeditor::diagnostics::testing
{
enum class Activity { component, job, worker, providers, snapshot, identity, nativeMetadata,
    reproduction, publication, validation, codecProbe, deviceProbe, count };
using ActivitySnapshot = std::array<std::uint64_t, static_cast<std::size_t>(Activity::count)>;
inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Activity::count)> activityCounts {};
inline void record(Activity activity) noexcept
{ activityCounts[static_cast<std::size_t>(activity)].fetch_add(1, std::memory_order_relaxed); }
inline ActivitySnapshot activitySnapshot() noexcept
{
    ActivitySnapshot result {};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = activityCounts[i].load(std::memory_order_relaxed);
    return result;
}
}
#define SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(name) ::singleeditor::diagnostics::testing::record(::singleeditor::diagnostics::testing::Activity::name)
#else
#define SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(name) ((void) 0)
#endif
