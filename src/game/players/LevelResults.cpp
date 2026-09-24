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
    m_heights.fill(kInitialHeight);
    m_order = {0, 2, 1};
    m_next = 0;
    m_tickRemainder = 0;
    for (usize i = 0; i < m_targets.size(); ++i) {
        const f32 ratio = static_cast<f32>(std::max(0, results.totals[i])) /
                          (static_cast<f32>(std::max(0, maxima[i])) + 1.0f);
        m_targets[i] = std::clamp(std::floor(ratio * kMaxHeight), 64.0f, kMaxHeight);
    }
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&](usize a, usize b) { return m_targets[a] > m_targets[b]; });
}
void LevelTally::update(f64 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    // At 60 Hz the integer increment ticks + (ticks >> 1) is 1, not 1.5.
    // Keep presentation ticks independent of the host render rate; don't give a
    // second pile the remainder of the update that completes the first.
    m_tickRemainder += std::min(seconds, 60.0) * 60;
    const auto ticks = static_cast<s32>(std::floor(m_tickRemainder + 1e-9));
    m_tickRemainder -= ticks;
    for (s32 tick = 0; tick < ticks && !finished(); ++tick) {
        const usize i = m_order[m_next];
        m_heights[i] = std::min(m_heights[i] + 1, m_targets[i]);
        if (m_heights[i] >= m_targets[i]) {
            ++m_next;
        }
    }
}
f32 LevelTally::fraction(usize pile) const {
    return pile < m_targets.size()
               ? (m_heights[pile] - kInitialHeight) / (m_targets[pile] - kInitialHeight)
               : 0;
}
} // namespace gdl::game
