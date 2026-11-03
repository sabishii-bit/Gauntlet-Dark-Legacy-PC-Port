#include "game/rollback/SharedConsoleWords.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

std::vector<std::string> splitConsoleWords(std::string_view line) {
    std::vector<std::string> words;
    usize at = 0;
    while (at < line.size()) {
        const usize start = line.find_first_not_of(' ', at);
        if (start == std::string_view::npos) {
            break;
        }
        const usize end = std::min(line.find(' ', start), line.size());
        words.emplace_back(line.substr(start, end - start));
        at = end;
    }
    return words;
}

} // namespace gdl::game
