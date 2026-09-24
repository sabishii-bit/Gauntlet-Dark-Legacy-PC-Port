#include "game/players/LevelResults.h"

#include <algorithm>
#include <cmath>
namespace gdl::game {
LevelResults LevelResults::between(s32 player, const CharacterSave& entry,
                                   const CharacterSave& exit, s32 kills) {
    return {player,
            {std::max(0, exit.gold - entry.gold), std::max(0, kills),
             std::max(0, exit.experience() - entry.experience())}};
}
void LevelTally::start(const LevelResults& results, const std::array<s32, 3>& maxima) {
    m_results = results;
    m_heights.fill(0);
    m_order = {0, 2, 1};
    m_next = 0;
    for (usize i = 0; i < m_targets.size(); ++i) {
        const f32 ratio = static_cast<f32>(std::max(0, results.totals[i])) /
                          (static_cast<f32>(std::max(0, maxima[i])) + 1.0f);
        m_targets[i] = std::clamp(ratio * 160.0f, 64.0f, 160.0f);
    }
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&](usize a, usize b) { return m_targets[a] > m_targets[b]; });
}
void LevelTally::update(f64 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    f32 growth = static_cast<f32>(std::min(seconds, 60.0)) * 90.0f;
    while (!finished() && growth > 0) {
        const usize i = m_order[m_next];
        const f32 added = std::min(growth, m_targets[i] - m_heights[i]);
        m_heights[i] += added;
        growth -= added;
        if (m_heights[i] >= m_targets[i]) {
            ++m_next;
        }
    }
}
f32 LevelTally::fraction(usize pile) const {
    return pile < m_targets.size() && m_targets[pile] > 0 ? m_heights[pile] / m_targets[pile] : 0;
}
} // namespace gdl::game
