#include "Automation/OperationRegistry.h"

namespace singleeditor
{
bool OperationRegistry::add(std::unique_ptr<AudioOperation> operation)
{
    if (operation == nullptr)
        return false;

    const auto descriptor = operation->descriptor();
    if (descriptor.id.isEmpty()
        || descriptor.schemaVersion <= 0
        || find(descriptor.id) != nullptr)
    {
        return false;
    }

    operations.push_back(std::move(operation));
    return true;
}

const AudioOperation* OperationRegistry::find(const juce::String& id) const noexcept
{
    for (const auto& operation : operations)
        if (operation->descriptor().id == id)
            return operation.get();

    return nullptr;
}

juce::Array<AudioOperationDescriptor> OperationRegistry::descriptors() const
{
    juce::Array<AudioOperationDescriptor> result;
    for (const auto& operation : operations)
        result.add(operation->descriptor());
    return result;
}
} // namespace singleeditor
