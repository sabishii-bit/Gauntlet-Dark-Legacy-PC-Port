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
};
} // namespace gdl::game
