#pragma once

#include <span>

#include "engine/core/Types.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {
/** Item lifetime and immediate-use effects, separate from their combat and visual consumers. */
class PlayerPowerups {
public:
    enum class Clock : u8 { Paused, Level, BossFight };
    static void update(std::span<PlayerRuntime> players, f32 seconds, Clock clock);
    /** One standing bearer affects the level; hidden bodies still carry their items. */
    static bool timeStopped(std::span<const PlayerRuntime> players);
};
} // namespace gdl::game
