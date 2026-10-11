#include "UI/Commands/CommandPaletteModel.h"

#include <algorithm>
#include <cctype>

namespace singleeditor
{
namespace
{
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

int scoreText(const std::string& haystack, const std::string& query)
{
    if (query.empty())
        return 1;
    if (haystack == query)
        return 1000;
    if (haystack.starts_with(query))
        return 800 - static_cast<int>(haystack.size() - query.size());
    if (const auto position = haystack.find(query); position != std::string::npos)
        return 600 - static_cast<int>(position);

    std::size_t cursor = 0;
    int gaps = 0;
    for (const auto character : query)
    {
        const auto found = haystack.find(character, cursor);
        if (found == std::string::npos)
            return 0;
        gaps += static_cast<int>(found - cursor);
        cursor = found + 1;
    }
    return std::max(1, 300 - gaps);
}
}

void CommandPaletteModel::open(const GuiCommandRegistry& registry,
                               const GuiCommandDispatchContext& context)
{
    index = registry.list(context, true);
    index.erase(std::remove_if(index.begin(), index.end(), [] (const GuiCommandSnapshot& item)
    {
        return !item.state.visible;
    }), index.end());
    opened = true;
    ++builds;
}

void CommandPaletteModel::close()
{
    std::vector<GuiCommandSnapshot>().swap(index);
    opened = false;
}

bool CommandPaletteModel::isOpen() const noexcept { return opened; }
std::size_t CommandPaletteModel::indexSize() const noexcept { return index.size(); }
std::size_t CommandPaletteModel::buildCount() const noexcept { return builds; }

std::vector<CommandPaletteMatch> CommandPaletteModel::search(std::string query,
                                                              std::size_t limit) const
{
    std::vector<CommandPaletteMatch> result;
    if (!opened || limit == 0)
        return result;

    query = lower(std::move(query));
    for (const auto& command : index)
    {
        auto score = std::max(scoreText(lower(command.state.label), query),
                              scoreText(lower(command.descriptor.id), query));
        for (const auto& term : command.descriptor.searchTerms)
            score = std::max(score, scoreText(lower(term), query) - 20);
        if (score > 0)
            result.push_back({ command, score });
    }

    std::stable_sort(result.begin(), result.end(), [] (const CommandPaletteMatch& left,
                                                       const CommandPaletteMatch& right)
    {
        if (left.score != right.score)
            return left.score > right.score;
        if (left.command.state.label != right.command.state.label)
            return left.command.state.label < right.command.state.label;
        return left.command.descriptor.id < right.command.descriptor.id;
    });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}
} // namespace singleeditor
