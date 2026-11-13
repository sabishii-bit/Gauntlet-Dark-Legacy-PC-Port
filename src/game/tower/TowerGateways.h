#pragma once

#include <span>
#include <string>

#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"

namespace gdl::game {

/** A world past the campaign's own that an optional expansion leads to through Sumner's
 * gateways. */
struct GatewayWorld {
    u8 id = 0;           ///< which world: its expansion's own number for it, on every install
    std::string label;   ///< the text id of its name among Sumner's choices
    bool usable = false; ///< whether this installation holds everything its tower needs

    bool operator==(const GatewayWorld&) const = default;
};

/** The worlds Sumner's gateways lead to, as the expansions built into the game offer them.
 * Nothing is looked for until asked: never as the game starts, and never online, where an open
 * scene is given none. */
class TowerGateways {
public:
    virtual ~TowerGateways() = default;
    /** Every world offered, usable on this installation or not, in the order Sumner names them;
     * each ask looks on disk afresh. */
    virtual std::span<const GatewayWorld> worlds() = 0;
};

/** Whether `save`'s character has beaten Garm, which wakes the gateways: its class holds his
 * piece, the battlefield's place in the tower's order (0x80124D14), which BossDeath gives every
 * player standing as he falls (PlayerGiveRune 0x800A2500). */
bool beatenGarm(const CharacterSave& save);

} // namespace gdl::game
