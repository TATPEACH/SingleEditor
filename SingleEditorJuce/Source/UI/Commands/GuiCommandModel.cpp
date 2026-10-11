#include "UI/Commands/GuiCommandModel.h"

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <utility>

namespace singleeditor
{
namespace
{
bool isCancelled(const GuiCommandDispatchContext& context) noexcept
{
    return context.cancellation != nullptr && context.cancellation->isCancelled();
}
}

GuiCommandResult GuiCommandResult::committed(std::string feedback)
{
    return { GuiCommandStatus::committed, std::move(feedback) };
}

GuiCommandResult GuiCommandResult::noChange(std::string feedback)
{
    return { GuiCommandStatus::noChange, std::move(feedback) };
}

GuiCommandResult GuiCommandResult::failed(std::string feedback)
{
    return { GuiCommandStatus::failed, std::move(feedback) };
}

GuiCommandRegistrationResult GuiCommandRegistry::registerProvider(GuiCommandProvider provider)
{
    if (provider.ownerId.empty() || provider.commands.empty() || !provider.resolveState
        || !provider.dispatch)
    {
        return { GuiCommandRegistrationStatus::invalidProvider, {} };
    }

    if (std::any_of(providers.begin(), providers.end(), [&provider] (const RegisteredProvider& item)
        {
            return item.provider.ownerId == provider.ownerId;
        }))
    {
        return { GuiCommandRegistrationStatus::duplicateOwner, provider.ownerId };
    }

    std::unordered_set<std::string> localIds;
    for (const auto& command : provider.commands)
    {
        if (!isValidCommandId(command.id) || command.label.empty()
            || !localIds.insert(command.id).second)
        {
            return { GuiCommandRegistrationStatus::invalidProvider, command.id };
        }

        for (const auto& existing : providers)
        {
            const auto duplicate = std::any_of(existing.provider.commands.begin(),
                                               existing.provider.commands.end(),
                                               [&command] (const GuiCommandDescriptor& item)
            {
                return item.id == command.id;
            });
            if (duplicate)
                return { GuiCommandRegistrationStatus::duplicateCommand, command.id };
        }
    }

    providers.push_back({ std::move(provider) });
    return { GuiCommandRegistrationStatus::registered, {} };
}

bool GuiCommandRegistry::unregisterProvider(std::string_view ownerId)
{
    const auto before = providers.size();
    providers.erase(std::remove_if(providers.begin(), providers.end(), [ownerId] (const RegisteredProvider& item)
    {
        return item.provider.ownerId == ownerId;
    }), providers.end());
    return providers.size() != before;
}

void GuiCommandRegistry::clear()
{
    providers.clear();
}

void GuiCommandRegistry::setActiveDocumentGeneration(std::uint64_t generation) noexcept
{
    activeDocumentGeneration = generation;
}

std::uint64_t GuiCommandRegistry::getActiveDocumentGeneration() const noexcept
{
    return activeDocumentGeneration;
}

std::size_t GuiCommandRegistry::providerCount() const noexcept
{
    return providers.size();
}

std::size_t GuiCommandRegistry::commandCount() const noexcept
{
    std::size_t result = 0;
    for (const auto& item : providers)
        result += item.provider.commands.size();
    return result;
}

std::optional<GuiCommandSnapshot> GuiCommandRegistry::resolve(
    std::string_view commandId,
    const GuiCommandDispatchContext& context) const
{
    for (const auto& item : providers)
    {
        const auto found = std::find_if(item.provider.commands.begin(), item.provider.commands.end(),
                                        [commandId] (const GuiCommandDescriptor& command)
        {
            return command.id == commandId;
        });
        if (found == item.provider.commands.end())
            continue;

        auto state = item.provider.resolveState(commandId, context);
        if (state.label.empty())
            state.label = found->label;
        if (!state.enabled && state.disabledReason.empty())
            state.disabledReason = "Command is currently unavailable.";
        return GuiCommandSnapshot { *found, std::move(state), item.provider.ownerId };
    }
    return std::nullopt;
}

std::vector<GuiCommandSnapshot> GuiCommandRegistry::list(
    const GuiCommandDispatchContext& context,
    bool discoverableOnly) const
{
    std::vector<GuiCommandSnapshot> result;
    result.reserve(commandCount());
    for (const auto& item : providers)
    {
        for (const auto& command : item.provider.commands)
        {
            if (discoverableOnly && !command.discoverable)
                continue;
            auto state = item.provider.resolveState(command.id, context);
            if (state.label.empty())
                state.label = command.label;
            if (!state.enabled && state.disabledReason.empty())
                state.disabledReason = "Command is currently unavailable.";
            result.push_back({ command, std::move(state), item.provider.ownerId });
        }
    }

    std::stable_sort(result.begin(), result.end(), [] (const GuiCommandSnapshot& left,
                                                       const GuiCommandSnapshot& right)
    {
        if (left.descriptor.domain != right.descriptor.domain)
            return left.descriptor.domain < right.descriptor.domain;
        if (left.state.label != right.state.label)
            return left.state.label < right.state.label;
        return left.descriptor.id < right.descriptor.id;
    });
    return result;
}

GuiCommandResult GuiCommandRegistry::dispatch(
    std::string_view commandId,
    const GuiCommandDispatchContext& context) const
{
    if (isCancelled(context))
        return { GuiCommandStatus::cancelled, "Command was cancelled before dispatch." };
    if (context.documentGeneration != 0
        && context.documentGeneration != activeDocumentGeneration)
    {
        return { GuiCommandStatus::staleGeneration,
                 "Command belongs to an inactive document generation." };
    }

    for (const auto& item : providers)
    {
        const auto found = std::find_if(item.provider.commands.begin(), item.provider.commands.end(),
                                        [commandId] (const GuiCommandDescriptor& command)
        {
            return command.id == commandId;
        });
        if (found == item.provider.commands.end())
            continue;

        auto state = item.provider.resolveState(commandId, context);
        if (!state.visible)
            return { GuiCommandStatus::hidden, "Command is not available in this context." };
        if (!state.enabled)
        {
            return { GuiCommandStatus::disabled,
                     state.disabledReason.empty() ? "Command is currently unavailable."
                                                  : std::move(state.disabledReason) };
        }

        try
        {
            auto result = item.provider.dispatch(commandId, context);
            if (!result.succeeded() && result.feedback.empty())
                result.feedback = "Command failed without changing product state.";
            return result;
        }
        catch (const std::exception&)
        {
            return { GuiCommandStatus::failed,
                     "Command failed without changing product state." };
        }
        catch (...)
        {
            return { GuiCommandStatus::failed,
                     "Command failed without changing product state." };
        }
    }

    return { GuiCommandStatus::notFound, "Unknown command." };
}

bool GuiCommandRegistry::isValidCommandId(std::string_view id) noexcept
{
    if (id.size() < 3 || id.front() == '.' || id.back() == '.' || id.find('.') == std::string_view::npos)
        return false;

    bool previousDot = false;
    for (const auto character : id)
    {
        const auto valid = (character >= 'a' && character <= 'z')
                        || (character >= '0' && character <= '9')
                        || character == '-' || character == '.';
        if (!valid || (character == '.' && previousDot))
            return false;
        previousDot = character == '.';
    }
    return true;
}
} // namespace singleeditor
