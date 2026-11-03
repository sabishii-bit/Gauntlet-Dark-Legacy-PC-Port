#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Expected.h"
#include "engine/core/Types.h"

#include "game/console/ConsoleCommand.h"
#include "game/critters/CombatantKind.h"

namespace gdl::game {

/** What `spawn` is asked to put before the character. */
struct SpawnRequest {
    enum class Kind : u8 {
        Enemy,     ///< `count` of the swarm's `enemyKind` at `tier`
        Critter,   ///< one `critter`, a gargoyle in `form`
        Generator, ///< one breeding `enemyKind` at `tier`
        Item,      ///< a pickup of the level in play, by its record's `name`
        Object,    ///< a copy of a level's object, by `name`
        Thing      ///< `name` as a pickup, failing that an object
    };
    static constexpr s32 kMostSpawned = 20;
    static constexpr s32 kMostTier = 3;
    static constexpr std::string_view kGargoylePrefix = "GAR_";
    /** A gargoyle's forms, the first the one it takes unless another is named. */
    static constexpr std::array<std::string_view, 3> kGargoyleForms{"EAGL", "LION", "SERP"};

    Kind kind = Kind::Thing;
    s32 enemyKind = -1;
    bool brood = false; ///< a generator of the temple's or the underworld's mixed kinds
    s32 tier = 1;
    s32 count = 1;
    CombatantKind critter = CombatantKind::Unknown;
    std::string form;               ///< a gargoyle's ("GAR_EAGL")
    std::string name;               ///< a thing's, as the level's records spell it
    std::optional<std::string> map; ///< the level it comes from, else the one in play
};

std::string spawnUsage();
/** Reads `spawn`'s arguments: a category word (enemy, critter, generator, item, object) may
 * come first, else the name says which it is; a map may come last, or first with an object.
 * The usage or the reason when they make no sense. */
gdl::Expected<SpawnRequest, std::string> parseSpawn(ConsoleCommand::Arguments arguments);
/** Whether the swarm can be asked for `kind`: its tiers, Death and IT; the great ones are
 * critters. */
bool spawnableEnemy(s32 kind);
/** A layout's generator record named `name` in any case, or null. */
const ItemInfo* generatorRecord(const WorldLayout& layout, std::string_view name);

} // namespace gdl::game
