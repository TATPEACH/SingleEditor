#pragma once

#include <cstdint>
#include <string_view>

namespace singleeditor
{
enum class RendererKind
{
    modern = 0,
    classic
};

struct RendererResourceAuditSnapshot final
{
    RendererKind selected = RendererKind::modern;
    bool selectionSealed = false;
    std::uint64_t modernRendererObjects = 0;
    std::uint64_t classicRendererObjects = 0;
    std::uint64_t modernTokenSliceAccesses = 0;
    std::uint64_t classicTokenSliceAccesses = 0;
    std::uint64_t modernAssetLookups = 0;
    std::uint64_t classicAssetLookups = 0;
    std::uint64_t modernMotionStarts = 0;
    std::uint64_t classicMotionStarts = 0;
    std::uint64_t rejectedSelectionChanges = 0;
};

class RendererSelection final
{
public:
    static bool select(RendererKind kind) noexcept;
    static void seal() noexcept;
    [[nodiscard]] static RendererKind selected() noexcept;
    [[nodiscard]] static bool isSealed() noexcept;
    [[nodiscard]] static std::string_view id(RendererKind kind) noexcept;
    [[nodiscard]] static RendererKind parse(std::string_view value,
                                            RendererKind fallback = RendererKind::modern) noexcept;

    static void noteRendererObject(RendererKind kind) noexcept;
    static void noteTokenSliceAccess(RendererKind kind) noexcept;
    static void noteAssetLookup(RendererKind kind) noexcept;
    static void noteMotionStart(RendererKind kind) noexcept;
    [[nodiscard]] static RendererResourceAuditSnapshot auditSnapshot() noexcept;

    // Pure-test seam. Production calls select once and then seals at factory creation.
    static void resetForTests(RendererKind kind = RendererKind::modern) noexcept;
};
} // namespace singleeditor
