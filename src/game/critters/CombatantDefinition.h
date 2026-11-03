#pragma once
#include <string>

#include "engine/core/Types.h"

#include "game/critters/CombatantKind.h"

namespace gdl::game {
/** Family policy supplied to shared combat, checked against the loaded descriptor family.
 * Every field is a function of `name` and `kind`: the critters' stock cache and the family
 * identity digest rely on it. */
struct CombatantDefinition {
    enum class Selection : u8 { Priority, Patterns };
    /** Whom it chooses among the players it sees. */
    enum class Targeting : u8 {
        Best,        ///< the best-scored player alone
        AngerRanked, ///< every player in sight, ranked by score weighed by its anger at each
    };
    /** How a hit's harm and worth are scaled. */
    enum class HarmScale : u8 {
        HitterLevel, ///< a hitter under the place's level does a little less
        PartySize,   ///< the more there are in the game, the less it takes and the more it pays
    };
    /** How it leaves once dead. */
    enum class Departure : u8 {
        Drops, ///< leaves its drop as its death ends
        Fades, ///< fades away without one
    };
    std::string name;
    CombatantKind kind = CombatantKind::Unknown;
    bool realmCostume = false;
    Selection selection = Selection::Priority;
    bool boundsToHome = false;
    bool projectileEndVisual = false; ///< show the impact artwork on otherwise silent flight ends
    f32 knockbackReduction = 0;
    bool breaksItems = false; ///< walks through chests and breaks barrels (fn_8005D5C8)
    bool patrols = false;   ///< walks the level's lookouts until it finds a player (CritterNewInst)
    bool healthBar = false; ///< show a floating meter even when the native type does not request it
    std::string dropForm;
    Targeting targeting = Targeting::Best;
    bool seesInvisible = false; ///< targets players whom invisibility hides from the rest
    HarmScale harmScale = HarmScale::HitterLevel;
    bool shrinks = true;        ///< enemy shrinkers double its harm taken and halve its blows
    bool shoved = true;         ///< hits push it by their flags
    bool tramplesSwarm = false; ///< walks over the swarm's short ones, hurting them
    Departure departure = Departure::Drops;
};
} // namespace gdl::game
