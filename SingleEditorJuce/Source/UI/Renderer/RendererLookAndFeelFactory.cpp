#include "UI/Renderer/RendererLookAndFeelFactory.h"

#include "UI/LookAndFeel/ClassicLookAndFeel.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

namespace singleeditor
{
std::unique_ptr<juce::LookAndFeel_V4> createSelectedLookAndFeel()
{
    RendererSelection::seal();
    if (RendererSelection::selected() == RendererKind::classic)
        return std::make_unique<ClassicLookAndFeel>();
    return std::make_unique<ModernAppleLookAndFeel>();
}
} // namespace singleeditor
