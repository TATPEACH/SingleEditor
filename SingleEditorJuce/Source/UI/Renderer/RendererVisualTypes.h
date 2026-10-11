#pragma once

namespace singleeditor
{
enum class RendererDensity { compact = 0, standard, expanded };
enum class RendererMagSize { automatic = 0, small, medium, large };
enum class RendererInformationTier { summary = 0, standard, detail };
enum class RendererEdge { north = 0, east, south, west };
enum class RendererAssetOwner { core = 0, module };

enum class RendererAssetId
{
    unknown = 0,
    openFiles,
    remove,
    preferences,
    save,
    skipBack,
    previousFrame,
    nextFrame,
    play,
    pause,
    stop,
    toggle,
    loop,
    fadeIn,
    fadeOut,
    undo,
    redo,
    reset,
    zoomIn,
    zoomOut,
    waveform,
    spectrum
};

struct RendererContentRect final
{
    int width = 0;
    int height = 0;
};

struct RendererAssetToken final
{
    RendererAssetId id = RendererAssetId::unknown;
    const char* fileName = "";
    RendererAssetOwner owner = RendererAssetOwner::core;
};
} // namespace singleeditor
