#pragma once

#include "UI/Renderer/RendererVisualTypes.h"

namespace singleeditor
{
class WorkspaceVisualContractTarget
{
public:
    virtual ~WorkspaceVisualContractTarget() = default;

    virtual void applyWorkspaceVisualContract(RendererDensity density,
                                              RendererMagSize size,
                                              RendererInformationTier tier,
                                              bool sweetSpotSatisfied) = 0;
};
} // namespace singleeditor
