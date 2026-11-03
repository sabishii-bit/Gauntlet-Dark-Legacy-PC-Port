#include "game/console/ConsoleUsage.h"

#include <algorithm>
#include <format>

#include "engine/core/Types.h"

namespace gdl::game {

std::string consoleUsage(std::span<const std::pair<std::string_view, std::string_view>> rows,
                         std::span<const std::string_view> notes) {
    usize widest = 0;
    for (const auto& [form, what] : rows) {
        widest = std::max(widest, form.size());
    }
    std::string text;
    for (const auto& [form, what] : rows) {
        if (!text.empty()) {
            text += '\n';
        }
        text += std::format("{:<{}}  {}", form, widest, what);
    }
    for (const std::string_view note : notes) {
        text += '\n';
        text += note;
    }
    return text;
}

} // namespace gdl::game
