#pragma once

#include "UI/Commands/GuiCommandModel.h"

#include <cstddef>
#include <string>
#include <vector>

namespace singleeditor
{
struct CommandPaletteMatch
{
    GuiCommandSnapshot command;
    int score = 0;
};

class CommandPaletteModel final
{
public:
    void open(const GuiCommandRegistry& registry,
              const GuiCommandDispatchContext& context);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] std::size_t indexSize() const noexcept;
    [[nodiscard]] std::size_t buildCount() const noexcept;
    [[nodiscard]] std::vector<CommandPaletteMatch> search(std::string query,
                                                           std::size_t limit = 50) const;

private:
    std::vector<GuiCommandSnapshot> index;
    bool opened = false;
    std::size_t builds = 0;
};
} // namespace singleeditor
