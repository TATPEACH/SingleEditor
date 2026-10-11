#include "UI/Renderer/RendererSelection.h"

#include <atomic>
#include <cctype>
#include <string>

namespace singleeditor
{
namespace
{
std::atomic<RendererKind> selectedRenderer { RendererKind::modern };
std::atomic_bool selectionSealed { false };
std::atomic_uint64_t modernRendererObjects { 0 };
std::atomic_uint64_t classicRendererObjects { 0 };
std::atomic_uint64_t modernTokenSliceAccesses { 0 };
std::atomic_uint64_t classicTokenSliceAccesses { 0 };
std::atomic_uint64_t modernAssetLookups { 0 };
std::atomic_uint64_t classicAssetLookups { 0 };
std::atomic_uint64_t modernMotionStarts { 0 };
std::atomic_uint64_t classicMotionStarts { 0 };
std::atomic_uint64_t rejectedSelectionChanges { 0 };

std::string lower(std::string_view text)
{
    std::string result(text);
    for (auto& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}
}

bool RendererSelection::select(RendererKind kind) noexcept
{
    if (selectionSealed.load(std::memory_order_acquire))
    {
        if (selectedRenderer.load(std::memory_order_relaxed) != kind)
        {
            rejectedSelectionChanges.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    selectedRenderer.store(kind, std::memory_order_release);
    return true;
}

void RendererSelection::seal() noexcept
{
    selectionSealed.store(true, std::memory_order_release);
}

RendererKind RendererSelection::selected() noexcept
{
    return selectedRenderer.load(std::memory_order_acquire);
}

bool RendererSelection::isSealed() noexcept
{
    return selectionSealed.load(std::memory_order_acquire);
}

std::string_view RendererSelection::id(RendererKind kind) noexcept
{
    return kind == RendererKind::classic ? "classic" : "modern";
}

RendererKind RendererSelection::parse(std::string_view value, RendererKind fallback) noexcept
{
    const auto normalized = lower(value);
    if (normalized == "classic")
        return RendererKind::classic;
    if (normalized == "modern")
        return RendererKind::modern;
    return fallback;
}

void RendererSelection::noteRendererObject(RendererKind kind) noexcept
{
    (kind == RendererKind::classic ? classicRendererObjects : modernRendererObjects)
        .fetch_add(1, std::memory_order_relaxed);
}

void RendererSelection::noteTokenSliceAccess(RendererKind kind) noexcept
{
    (kind == RendererKind::classic ? classicTokenSliceAccesses : modernTokenSliceAccesses)
        .fetch_add(1, std::memory_order_relaxed);
}

void RendererSelection::noteAssetLookup(RendererKind kind) noexcept
{
    (kind == RendererKind::classic ? classicAssetLookups : modernAssetLookups)
        .fetch_add(1, std::memory_order_relaxed);
}

void RendererSelection::noteMotionStart(RendererKind kind) noexcept
{
    (kind == RendererKind::classic ? classicMotionStarts : modernMotionStarts)
        .fetch_add(1, std::memory_order_relaxed);
}

RendererResourceAuditSnapshot RendererSelection::auditSnapshot() noexcept
{
    return {
        selected(),
        isSealed(),
        modernRendererObjects.load(std::memory_order_relaxed),
        classicRendererObjects.load(std::memory_order_relaxed),
        modernTokenSliceAccesses.load(std::memory_order_relaxed),
        classicTokenSliceAccesses.load(std::memory_order_relaxed),
        modernAssetLookups.load(std::memory_order_relaxed),
        classicAssetLookups.load(std::memory_order_relaxed),
        modernMotionStarts.load(std::memory_order_relaxed),
        classicMotionStarts.load(std::memory_order_relaxed),
        rejectedSelectionChanges.load(std::memory_order_relaxed)
    };
}

void RendererSelection::resetForTests(RendererKind kind) noexcept
{
    selectionSealed.store(false, std::memory_order_release);
    selectedRenderer.store(kind, std::memory_order_release);
    modernRendererObjects.store(0, std::memory_order_relaxed);
    classicRendererObjects.store(0, std::memory_order_relaxed);
    modernTokenSliceAccesses.store(0, std::memory_order_relaxed);
    classicTokenSliceAccesses.store(0, std::memory_order_relaxed);
    modernAssetLookups.store(0, std::memory_order_relaxed);
    classicAssetLookups.store(0, std::memory_order_relaxed);
    modernMotionStarts.store(0, std::memory_order_relaxed);
    classicMotionStarts.store(0, std::memory_order_relaxed);
    rejectedSelectionChanges.store(0, std::memory_order_relaxed);
}
} // namespace singleeditor
