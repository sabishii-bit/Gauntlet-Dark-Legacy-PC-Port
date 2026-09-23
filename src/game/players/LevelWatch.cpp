#include "game/players/LevelWatch.h"

#include <algorithm>

namespace gdl::game {

std::optional<LevelChange> LevelWatch::observe(int player, int level) {
    const auto mark = std::ranges::find(m_marks, player, &Mark::player);
    if (mark == m_marks.end()) {
        m_marks.push_back(Mark{player, level});
        return std::nullopt;
    }
    if (mark->level == level) {
        return std::nullopt;
    }
    LevelChange change;
    change.player = player;
    change.from = mark->level;
    change.to = level;
    mark->level = level;
    return change;
}

void LevelWatch::forget(int player) {
    std::erase_if(m_marks, [player](const Mark& mark) { return mark.player == player; });
}

std::optional<int> LevelWatch::markOf(int player) const {
    const auto mark = std::ranges::find(m_marks, player, &Mark::player);
    return mark == m_marks.end() ? std::nullopt : std::optional<int>(mark->level);
}

} // namespace gdl::game
