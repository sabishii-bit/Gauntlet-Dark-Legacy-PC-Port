#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The narrator's praise for a run of close blows (pmotion.c 2695, player.c 2553): each blow on
 * an enemy taller than four or on a great one counts one, or three when it kills or is not
 * taken; two seconds after the last, a run of thirty earns `S_HEROIC` and one of forty-five
 * `S_BRAVERY`, and the count starts over.
 */
class MeleeStreak {
public:
    static constexpr s32 kHurt = 1;     ///< a blow that hurts
    static constexpr s32 kTelling = 3;  ///< one that kills, or is not taken (damage_enemy's -1)
    static constexpr s32 kHeroic = 30;  ///< 0x1E
    static constexpr s32 kBravery = 45; ///< 0x2D
    static constexpr f32 kQuiet = 2.0f; ///< seconds after the last blow
    static constexpr f32 kWait = 3.0f;  ///< the most the praise waits for the narrator
    static constexpr std::string_view kHeroicVoice = "S_HEROIC";
    static constexpr std::string_view kBraveryVoice = "S_BRAVERY";

    /** A counted blow landed; `telling` when it killed or was not taken. */
    void record(bool telling);
    /** Runs the quiet on by `seconds`: the praise earned once it is over, else nothing. */
    std::string_view step(f32 seconds);
    s32 count() const { return m_count; }

private:
    s32 m_count = 0;
    f32 m_quiet = 0.0f;
    bool m_running = false;
};

} // namespace gdl::game
