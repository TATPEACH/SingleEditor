#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace singleeditor
{
enum class GuiCommandDomain { core, document, workspace, module };
enum class GuiCommandSource { toolbar, menu, shortcut, mag, palette, accessibility, nativeWindow };
enum class GuiCommandStatus
{
    committed,
    noChange,
    disabled,
    hidden,
    notFound,
    staleGeneration,
    cancelled,
    conflict,
    failed
};

struct GuiCommandState
{
    bool enabled = true;
    bool checked = false;
    bool visible = true;
    std::string label;
    std::string disabledReason;
};

class GuiCommandCancellation final
{
public:
    void cancel() noexcept { cancelled.store(true, std::memory_order_release); }
    [[nodiscard]] bool isCancelled() const noexcept
    {
        return cancelled.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> cancelled { false };
};

struct GuiCommandDispatchContext
{
    std::uint64_t documentGeneration = 0;
    GuiCommandSource source = GuiCommandSource::shortcut;
    std::string targetScope;
    std::shared_ptr<const GuiCommandCancellation> cancellation;
};

struct GuiCommandResult
{
    GuiCommandStatus status = GuiCommandStatus::failed;
    std::string feedback;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == GuiCommandStatus::committed || status == GuiCommandStatus::noChange;
    }

    static GuiCommandResult committed(std::string feedback = {});
    static GuiCommandResult noChange(std::string feedback = {});
    static GuiCommandResult failed(std::string feedback);
};

struct GuiCommandDescriptor
{
    std::string id;
    GuiCommandDomain domain = GuiCommandDomain::core;
    std::string label;
    std::string shortcut;
    std::vector<std::string> searchTerms;
    bool discoverable = true;
};

struct GuiCommandSnapshot
{
    GuiCommandDescriptor descriptor;
    GuiCommandState state;
    std::string ownerId;
};

struct GuiCommandProvider
{
    using StateResolver = std::function<GuiCommandState(std::string_view,
                                                         const GuiCommandDispatchContext&)>;
    using Handler = std::function<GuiCommandResult(std::string_view,
                                                    const GuiCommandDispatchContext&)>;

    std::string ownerId;
    std::vector<GuiCommandDescriptor> commands;
    StateResolver resolveState;
    Handler dispatch;
};

enum class GuiCommandRegistrationStatus
{
    registered,
    invalidProvider,
    duplicateOwner,
    duplicateCommand
};

struct GuiCommandRegistrationResult
{
    GuiCommandRegistrationStatus status = GuiCommandRegistrationStatus::invalidProvider;
    std::string conflictId;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == GuiCommandRegistrationStatus::registered;
    }
};

class GuiCommandRegistry final
{
public:
    GuiCommandRegistrationResult registerProvider(GuiCommandProvider provider);
    bool unregisterProvider(std::string_view ownerId);
    void clear();

    void setActiveDocumentGeneration(std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t getActiveDocumentGeneration() const noexcept;
    [[nodiscard]] std::size_t providerCount() const noexcept;
    [[nodiscard]] std::size_t commandCount() const noexcept;

    [[nodiscard]] std::optional<GuiCommandSnapshot> resolve(
        std::string_view commandId,
        const GuiCommandDispatchContext& context) const;
    [[nodiscard]] std::vector<GuiCommandSnapshot> list(
        const GuiCommandDispatchContext& context,
        bool discoverableOnly = false) const;
    GuiCommandResult dispatch(std::string_view commandId,
                              const GuiCommandDispatchContext& context) const;

    static bool isValidCommandId(std::string_view id) noexcept;

private:
    struct RegisteredProvider { GuiCommandProvider provider; };
    std::vector<RegisteredProvider> providers;
    std::uint64_t activeDocumentGeneration = 0;
};
} // namespace singleeditor
