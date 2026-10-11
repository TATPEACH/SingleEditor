#pragma once
#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"
#include <SingleEditorCoreActionIconData.h>
#include <string_view>

namespace singleeditor
{
struct CoreActionIconBytes final
{
    const char* data = nullptr;
    int size = 0;
};

// A bounded lookup over only the approved built-in Core glyphs. No file lookup,
// runtime payload catalog, resource pack read or image/font initialization.
inline CoreActionIconBytes coreActionIconBytes(RendererAssetId id) noexcept
{
    const auto token = ModernNorthStarVisualContract::assetToken(id);
    if (id == RendererAssetId::unknown || token.fileName[0] == '\0') return {};
    for (int i = 0; i < SingleEditorCoreIconData::namedResourceListSize; ++i)
    {
        if (std::string_view(token.fileName) == SingleEditorCoreIconData::originalFilenames[i])
        {
            CoreActionIconBytes result;
            result.data = SingleEditorCoreIconData::getNamedResource(SingleEditorCoreIconData::namedResourceList[i], result.size);
            return result;
        }
    }
    return {};
}
}
