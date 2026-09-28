#pragma once

#include <optional>

#include "engine/core/Types.h"

namespace gdl::game {

/** What a class family's potion magic learns to do to the level's items, by the class's place
 * in its family of four: the warrior's turns junk into treasure, the valkyrie's stops traps,
 * the wizard's cleanses spoiled food and the archer's shows up secret walls. */
enum class MagicPerkFamily : u8 { Treasure, Traps, Food, Walls };

/** What a perk did to an item, in the order of the lessons that announce each
 * (messages 0x8B to 0x92). */
enum class MagicPerkDeed : u8 {
    JunkToSilver,
    JunkToGold,
    StopTrap,
    DestroyTrap,
    CleanseFruit,
    CleanseMeat,
    RevealWall,
    DestroyWall
};

/** The perk a character's potion magic carries (start_magic, fn_8005BA1C): from level 25,
 * the greater one from 50. */
struct MagicPerk {
    static constexpr s32 kLevel = 25;
    static constexpr s32 kGreaterLevel = 50;

    MagicPerkFamily family = MagicPerkFamily::Treasure;
    bool greater = false;

    /** The perk of class `character` at `level`, or none under 25. */
    static std::optional<MagicPerk> of(s32 character, s32 level);
};

} // namespace gdl::game
