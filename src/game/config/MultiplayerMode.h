#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/** Whether the party's attacks leave other players alone, stun them, or hurt them. */
enum class MultiplayerMode : u8 { Normal, Stun, Hurt };

} // namespace gdl::game
