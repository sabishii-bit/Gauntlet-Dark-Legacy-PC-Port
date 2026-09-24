#pragma once
#include <array>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"
namespace gdl::game {
/** A completed level's gains, not lifetime totals. Gold/experience are already awarded:
 * the tally is presentation only and must never award them a second time. */
struct LevelResults {
    s32 player = 0;
    std::array<s32, 3> totals{}; ///< gold, kills (including generators), experience
    static LevelResults between(s32 player, const CharacterSave& entry, const CharacterSave& exit,
                                s32 kills);
};
/** Three piles grow in descending height order, with gold, experience, kills as tie order. */
class LevelTally {
public:
    void start(const LevelResults& results, const std::array<s32, 3>& maxima);
    void update(f64 seconds);
    f32 fraction(usize pile) const;
    bool finished() const { return m_next == m_order.size(); }
    const LevelResults& results() const { return m_results; }

private:
    LevelResults m_results;
    std::array<f32, 3> m_targets{};
    std::array<f32, 3> m_heights{};
    std::array<usize, 3> m_order{0, 2, 1};
    usize m_next = 0;
};
} // namespace gdl::game
