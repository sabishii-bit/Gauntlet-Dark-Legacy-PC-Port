#pragma once
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/FixtureRequest.h"
#include "game/level/LevelCatalog.h"

namespace gdl::game {

/** What `list` is asked for; a map's name may follow it. */
enum class Listing : u8 {
    Maps,
    Items,
    Inventory,
    Enemies,
    Critters,
    Generators,
    Objects,
    Switches,
    Characters,
    Unknown
};
Listing listingOf(ConsoleCommand::Arguments arguments);
/** The map named after the listing ("G1"), when one is. */
std::optional<std::string> listedMap(ConsoleCommand::Arguments arguments);
std::string listUsage();

/** Every item `get` knows, one line each with what it does. */
std::vector<std::string> itemLines();
/** Every realm's levels on a line of their own, by name and title. */
std::vector<std::string> mapLines(const LevelCatalog& catalog);
/** What a character is, carries and has gathered. */
std::vector<std::string> inventoryLines(const CharacterSave& save);
/** The swarm's kinds `spawn` takes, by the codes the levels name them. */
std::vector<std::string> enemyKindLines();
/** The great ones `spawn` takes. */
std::vector<std::string> critterLines();
/** A level's roster: the kinds its generators breed for each class of placement. */
std::vector<std::string> rosterLines(std::span<const LevelEnemy> roster);
/** A layout's generators, by the kind each names, with their tiers. */
std::vector<std::string> generatorLines(const WorldLayout& layout);
/** A layout's placed enemies, by name. */
std::vector<std::string> placedEnemyLines(const WorldLayout& layout);
/** The great ones a layout places, by name. */
std::vector<std::string> placedCritterLines(const WorldLayout& layout);
/** A layout's pickup records, by name and what each is. */
std::vector<std::string> pickupLines(const WorldLayout& layout);
/** A layout's objects: how many, and each distinct name with its count. */
std::vector<std::string> objectLines(const WorldLayout& layout);
std::vector<std::string> switchLines(std::span<const SwitchEntry> switches);
/** Every playable class with its standing: the starting eight, the unlockables locked or
 * unlocked by `unlocks`, Sumner hidden until unlocked; then the costumes a name selects. */
std::vector<std::string> characterLines(u16 unlocks);

} // namespace gdl::game
