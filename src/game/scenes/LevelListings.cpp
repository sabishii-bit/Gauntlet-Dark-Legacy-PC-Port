#include "game/scenes/LevelListings.h"

#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"

#include "game/characters/CharacterSave.h"
#include "game/console/ConsoleArguments.h"
#include "game/console/FixtureRequest.h"
#include "game/console/LevelListingLines.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/FixtureConsoleCommands.h"

namespace gdl::game {

namespace {
using Arguments = ConsoleListing::Arguments;
using Lines = std::vector<std::string>;

/** What a listing reads of a level: the one in play's, or a named map's loaded for it. */
struct ListedLevel {
    std::string name;
    const WorldLayout* layout = nullptr;
    std::span<const LevelEnemy> roster;
    WorldLayout loaded; ///< a named map's, when it is not the one in play
    WorldData data;
};

/** The map named after a listing ("G1"), when one is. */
std::optional<std::string> mapOf(Arguments rest) {
    return rest.empty() ? std::nullopt : std::optional{upperArgument(rest[0])};
}

/** Fills `level` for `map`, or for the level in play without one; what to say instead for
 * nothing in play, or a map unknown or not unpacked. */
std::optional<std::string> listedLevel(const LevelListingsAccess& access,
                                       const std::optional<std::string>& map, ListedLevel& level) {
    if (!map.has_value()) {
        const LevelWorld* world = access.viewed();
        if (world == nullptr) {
            return "Nothing is in play.";
        }
        level.name = world->ref().name;
        level.layout = &world->layout();
        if (const LevelInfo* info = world->level()) {
            level.roster = info->enemies;
        }
        return std::nullopt;
    }
    const std::optional<LevelRef> ref =
        *map == "TOWER" ? std::optional{LevelRef::tower()} : access.levels.byName(*map);
    if (!ref.has_value()) {
        return std::format("No map named {}; list maps names them", *map);
    }
    if (!level.loaded.load(access.unpackedRoot / ref->directory)) {
        return std::format("{} is not unpacked", ref->name);
    }
    level.name = ref->name;
    level.layout = &level.loaded;
    if (level.data.load(access.unpackedRoot / ref->worldDataFile())) {
        if (const LevelInfo* info = level.data.level(ref->name)) {
            level.roster = info->enemies;
        }
    }
    return std::nullopt;
}

void append(Lines& lines, const Lines& more) {
    lines.insert(lines.end(), more.begin(), more.end());
}

/** Adds the level's name and what `body` adds of it to `lines`, or why it cannot be listed. */
template <typename Body>
void addLevel(Lines& lines, const LevelListingsAccess& access,
              const std::optional<std::string>& map, Body&& body) {
    ListedLevel level;
    if (const std::optional<std::string> refused = listedLevel(access, map, level)) {
        lines.push_back(*refused);
        return;
    }
    lines.push_back(std::format("In {}:", level.name));
    std::forward<Body>(body)(level);
}

void addListing(ConsoleListings& listings, std::string word, ConsoleListing::Row row,
                LambdaListing::Lines lines) {
    listings.add(
        std::make_unique<LambdaListing>(std::move(word), std::move(row), std::move(lines)));
}
} // namespace

void registerLevelListings(ConsoleHost& host, ConsoleListings& listings,
                           const LevelListingsAccess& access) {
    host.add(listCommand(listings));
    addListing(listings, "maps", {"list maps", "every level, by realm"},
               [access](Arguments /*rest*/) { return mapLines(access.levels); });
    addListing(listings, "items", {"list items [map]", "what get knows, then a level's pickups"},
               [access](Arguments rest) {
                   const std::optional<std::string> map = mapOf(rest);
                   Lines lines;
                   if (!map.has_value()) {
                       lines = itemLines();
                   }
                   addLevel(lines, access, map, [&](const ListedLevel& level) {
                       append(lines, pickupLines(*level.layout));
                   });
                   return lines;
               });
    addListing(listings, "inventory", {"list inventory", "what your character is and carries"},
               [access](Arguments /*rest*/) {
                   const CharacterSave* save = access.save();
                   return save != nullptr ? inventoryLines(*save) : Lines{"Nobody is in."};
               });
    addListing(listings, "enemies",
               {"list enemies [map]", "the swarm's kinds, then a level's roster and generators"},
               [access](Arguments rest) {
                   const std::optional<std::string> map = mapOf(rest);
                   Lines lines;
                   if (!map.has_value()) {
                       lines = enemyKindLines();
                   }
                   addLevel(lines, access, map, [&](const ListedLevel& level) {
                       append(lines, rosterLines(level.roster));
                       append(lines, generatorLines(*level.layout));
                       append(lines, placedEnemyLines(*level.layout));
                   });
                   return lines;
               });
    addListing(listings, "critters",
               {"list critters [map]", "the great ones, then those a level places"},
               [access](Arguments rest) {
                   Lines lines = critterLines();
                   addLevel(lines, access, mapOf(rest), [&](const ListedLevel& level) {
                       append(lines, placedCritterLines(*level.layout));
                   });
                   return lines;
               });
    addListing(listings, "generators", {"list generators [map]", "a level's generators"},
               [access](Arguments rest) {
                   Lines lines;
                   addLevel(lines, access, mapOf(rest), [&](const ListedLevel& level) {
                       append(lines, generatorLines(*level.layout));
                       lines.emplace_back("spawn generator <enemy> [tier] stands another");
                   });
                   return lines;
               });
    addListing(
        listings, "objects", {"list objects [map]", "a level's objects"}, [access](Arguments rest) {
            Lines lines;
            addLevel(lines, access, mapOf(rest),
                     [&](const ListedLevel& level) { append(lines, objectLines(*level.layout)); });
            return lines;
        });
    addListing(listings, "switches", {"list switches [map]", "a level's switches, for activate"},
               [access](Arguments rest) {
                   const std::optional<std::string> map = mapOf(rest);
                   Lines lines;
                   // The level in play's switches stand as they are now, fired or not.
                   const LevelWorld* world = map.has_value() ? nullptr : access.viewed();
                   if (world != nullptr) {
                       lines.push_back(std::format("In {}:", world->ref().name));
                       append(lines, switchLines(switchesInPlay(*world)));
                       return lines;
                   }
                   addLevel(lines, access, map, [&](const ListedLevel& level) {
                       append(lines, switchLines(switchesOf(*level.layout)));
                   });
                   return lines;
               });
}

} // namespace gdl::game
