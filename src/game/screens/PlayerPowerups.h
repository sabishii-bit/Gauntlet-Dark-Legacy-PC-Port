#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {
/** A special's ending heard: whose, and the sound. */
struct PowerupEnding {
    usize player = 0; ///< the record's place in the party
    std::string_view sound;
};

/** Item lifetime and immediate-use effects, separate from their combat and visual consumers. */
class PlayerPowerups {
public:
    enum class Clock : u8 { Paused, Level, BossFight };
    /** Runs the clocks and the turbo refills, and reports the endings heard this update
     * (`PowerupEndings`, against each record's `wornSpecial`). */
    static std::vector<PowerupEnding> update(std::span<PlayerRuntime> players, f32 seconds,
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
