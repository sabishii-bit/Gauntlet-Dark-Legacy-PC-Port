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
    static constexpr f32 kBaseline = 320;
    static constexpr f32 kInitialHeight = 20;
    static constexpr f32 kMaxHeight = 208;
    void start(const LevelResults& results, const std::array<s32, 3>& maxima);
    void update(f64 seconds);
    f32 fraction(usize pile) const;
    f32 height(usize pile) const { return m_heights.at(pile); }
    f32 targetHeight(usize pile) const { return m_targets.at(pile); }
    const std::array<usize, 3>& order() const { return m_order; }
    usize growingRank() const { return m_next; }
    bool finished() const { return m_next == m_order.size(); }
    const LevelResults& results() const { return m_results; }

private:
    LevelResults m_results;
    std::array<f32, 3> m_targets{};
    std::array<f32, 3> m_heights{};
    std::array<usize, 3> m_order{0, 2, 1};
    usize m_next = 0;
    f64 m_tickRemainder = 0;
};
} // namespace gdl::game
