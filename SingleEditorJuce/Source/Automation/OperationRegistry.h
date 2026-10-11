#pragma once

#include "Automation/AudioOperation.h"

#include <memory>
#include <vector>

namespace singleeditor
{
class OperationRegistry final
{
public:
    bool add(std::unique_ptr<AudioOperation> operation);
    const AudioOperation* find(const juce::String& id) const noexcept;
    juce::Array<AudioOperationDescriptor> descriptors() const;

private:
    std::vector<std::unique_ptr<AudioOperation>> operations;
};
} // namespace singleeditor
