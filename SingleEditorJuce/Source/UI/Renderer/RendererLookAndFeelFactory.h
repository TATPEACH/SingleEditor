#pragma once

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/Renderer/RendererSelection.h"

namespace singleeditor
{
[[nodiscard]] std::unique_ptr<juce::LookAndFeel_V4> createSelectedLookAndFeel();
} // namespace singleeditor
