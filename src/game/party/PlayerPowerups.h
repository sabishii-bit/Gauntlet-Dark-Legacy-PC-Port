#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/party/PlayerRuntime.h"

namespace gdl::game {
/** A special's ending heard: whose, and the sound. */
struct PowerupEnding {
    usize partyIndex = 0;
    std::string_view sound;
};

/** Item lifetime and immediate-use effects, separate from their combat and visual consumers. */
class PlayerPowerups {
public:
    enum class Clock : u8 { Paused, Level, BossFight };
    /** How many times as fast as a level's a boss fight spends timed powerups
     * (PlayerProcessPowerups' double at 0x80347A40). */
    static constexpr f32 kBossFightRate = 3.0f;
    /** Seconds of a timed powerup spent for each second of `clock`. */
    static f32 rateOf(Clock clock);
    /** One update of the clock and of what is worn: runClock, then wear. */
    static std::vector<PowerupEnding> update(std::span<PlayerRuntime> players, f32 seconds,
                                             Clock clock);
    /** Spends the standing players' timed powerups at `clock`'s rate (PlayerProcessPowerups'
     * slot loop); a decoy's slot is spent as it is let loose, in wear. */
    static void runClock(std::span<PlayerRuntime> players, f32 seconds, Clock clock);
    /** Runs the turbo refills and the decoys, a decoy let loose spending its slot at `clock`'s
     * rate, and reports the endings heard this update (`PowerupEndings`, against each record's
     * `wornSpecial`). */
    static std::vector<PowerupEnding> wear(std::span<PlayerRuntime> players, f32 seconds,
                                           Clock clock);
    /** One standing bearer affects the level; hidden bodies still carry their items. */
    static bool timeStopped(std::span<const PlayerRuntime> players);
    /** Disable challenge-ineligible items for every participant without consuming them. */
    static void restrictToChallenge(std::span<PlayerRuntime> players);
    /** How many standing players wear a working enemy shrinker (SetPlayerVars' state 1). */
    static s32 enemyShrinkers(std::span<const PlayerRuntime> players);
    /** The scale the swarm and the great ones are held at by the shrinkers worn. */
    static f32 enemyShrink(std::span<const PlayerRuntime> players, bool bossEncounter);
};
} // namespace gdl::game
