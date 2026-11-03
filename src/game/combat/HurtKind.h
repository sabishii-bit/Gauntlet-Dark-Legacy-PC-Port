#pragma once

#include "engine/core/Types.h"

namespace gdl::game {
/** What hurt a character, which picks how it cries out. */
enum class HurtKind : u8 {
    Blow,      ///< cries out once enough has been taken
    Burn,      ///< always cries out
    Pierce,    ///< groans
    Gas,       ///< cloud damage: gas protection and queued pain, not the poison-trap voice
    QuietBlow, ///< accumulates pain; the attacker supplies the impact sound, without a cry
    DeathDrain ///< bypasses ordinary armor without knockback or impact audio
};
} // namespace gdl::game
