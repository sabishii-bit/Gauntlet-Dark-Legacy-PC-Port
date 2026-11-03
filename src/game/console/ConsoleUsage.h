#pragma once
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace gdl::game {

/** Forms in a column with what each does beside them, a line each, then any notes. */
std::string consoleUsage(std::span<const std::pair<std::string_view, std::string_view>> rows,
                         std::span<const std::string_view> notes = {});

} // namespace gdl::game
