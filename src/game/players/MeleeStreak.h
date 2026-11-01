#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

/**
 * The narrator's praise for a run of close blows (pmotion.c 2695, player.c 2553): each blow on
 * an enemy taller than four or on a great one counts one, or three when it kills or is not
 * taken; once the quiet after the last outlasts the window (the original's two seconds, or
 * what a mechanic's rules give, at most three), a run of thirty earns `S_HEROIC` and one of
 * forty-five `S_BRAVERY`, and the count starts over. What counts is decided by `BlowRules`.
 */
class MeleeStreak {
    friend void visitNativeState(NativeStateDigest& /*digest*/, const MeleeStreak& /*value*/);

public:
    bool operator==(const MeleeStreak&) const = default;
    struct Checkpoint {
        s32 count = 0;
        f32 quiet = 0;
        bool running = false;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    Checkpoint checkpoint() const { return {m_count, m_quiet, m_running}; }
    /** Does not advance the silence timer or emit praise. */
    bool restore(const Checkpoint& state);
    static constexpr s32 kHurt = 1;     ///< a blow that hurts
    static constexpr s32 kTelling = 3;  ///< one that kills, or is not taken (damage_enemy's -1)
    static constexpr s32 kHeroic = 30;  ///< 0x1E
    static constexpr s32 kBravery = 45; ///< 0x2D
    static constexpr f32 kOriginalQuiet = 2.0f; ///< the quiet a run survives in the original
    static constexpr f32 kLongestQuiet = 3.0f;  ///< the longest any rules may let it survive
    static constexpr f32 kWait = 3.0f;  ///< the most the praise waits for the narrator
    static constexpr std::string_view kHeroicVoice = "S_HEROIC";
    static constexpr std::string_view kBraveryVoice = "S_BRAVERY";

    /** A counted blow landed; `telling` when it killed or was not taken. */
    void record(bool telling);
    /** Runs the quiet on by `seconds`; the run is over once it outlasts `quiet` (at most
     * `kLongestQuiet`): the praise earned then, else nothing. */
    std::string_view step(f32 seconds, f32 quiet = kOriginalQuiet);
    s32 count() const { return m_count; }
    /** Seconds since the last blow; the run ends past the quiet it is given. */
    f32 quiet() const { return m_quiet; }
    bool running() const { return m_running; }

private:
    s32 m_count = 0;
    f32 m_quiet = 0.0f;
    bool m_running = false;
};

} // namespace gdl::game
