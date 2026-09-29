#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * How long a player has stood still on an exit for the rest of the party (idle_timer, DoExit):
 * at ten seconds, and every nine after, the narrator names them and says they wait
 * (AudioPlayerBreath, player.c 2487: `S_WAITINGL` in the tower, else `S_WAITING`).
 */
class ExitWait {
public:
    static constexpr s32 kFirstTicks = 600;
    static constexpr s32 kAgainTicks = 540; ///< taken off each time it speaks
    static constexpr f32 kWait = 5.0f;      ///< the most the line waits for the narrator
    static constexpr std::string_view kVoice = "S_WAITING";
    static constexpr std::string_view kTowerVoice = "S_WAITINGL";

    /** Runs on by `ticks` while `waiting`, else starts over; whether it is time to speak. */
    bool step(bool waiting, s32 ticks);
    s32 ticks() const { return m_ticks; }

private:
    s32 m_ticks = 0;
};

} // namespace gdl::game
