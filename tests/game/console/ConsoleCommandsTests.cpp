#include <algorithm>
#include <cctype>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/console/ConsoleCommands.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"
#include "game/players/ItemPickup.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Args = std::vector<std::string>;
using Kind = ConsoleCommands::Gift::Kind;

bool mentions(std::span<const std::string> lines, std::string_view text) {
    return std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); });
}

TEST_CASE("list knows maps, items and inventory in any case", "[dev-console][commands]") {
    CHECK(ConsoleCommands::listingOf(Args{"maps"}) == ConsoleCommands::Listing::Maps);
    CHECK(ConsoleCommands::listingOf(Args{"ITEMS"}) == ConsoleCommands::Listing::Items);
    CHECK(ConsoleCommands::listingOf(Args{"Inventory"}) == ConsoleCommands::Listing::Inventory);
    CHECK(ConsoleCommands::listingOf(Args{}) == ConsoleCommands::Listing::Unknown);
    CHECK(ConsoleCommands::listingOf(Args{"shops"}) == ConsoleCommands::Listing::Unknown);
    CHECK(ConsoleCommands::listingOf(Args{"Switches"}) == ConsoleCommands::Listing::Switches);
    CHECK(ConsoleCommands::listingOf(Args{"characters"}) == ConsoleCommands::Listing::Characters);
    CHECK(ConsoleCommands::listUsage().contains("list switches"));
    CHECK(ConsoleCommands::listUsage().contains("list characters"));
}

TEST_CASE("list characters names every class with its standing and the name costumes",
          "[dev-console][commands][characters]") {
    const auto lines = ConsoleCommands::characterLines(0);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "Starting: WAR, VAL, WIZ, ARC, DWF, KNI, SOR, JES");
    CHECK(lines[1].starts_with("Unlockable (unlock character <code>): MIN locked, "));
    CHECK(lines[1].ends_with("SUM hidden"));
    CHECK(lines[2].contains("CYBMOS (WAR HIR)"));
    const auto unlocked = ConsoleCommands::characterLines(0x101);
    CHECK(unlocked[1].contains("MIN unlocked"));
    CHECK(unlocked[1].contains("FAL locked"));
    CHECK(unlocked[1].ends_with("SUM unlocked"));
}

TEST_CASE("lock and unlock take a character by code or a door or chest by token or name",
          "[dev-console][commands]") {
    std::string error;
    const auto character =
        ConsoleCommands::parseLockRequest(Args{"Character", "min"}, error, "unlock");
    REQUIRE(character.has_value());
    CHECK(character->character);
    CHECK(character->what == "min");
    const auto fixture = ConsoleCommands::parseLockRequest(Args{"gate#2"}, error, "lock");
    REQUIRE(fixture.has_value());
    CHECK_FALSE(fixture->character);
    CHECK(fixture->what == "gate#2");
    for (const Args& args : {Args{}, Args{"character"}, Args{"a", "b"}, Args{"a", "b", "c"}}) {
        CAPTURE(args.size());
        CHECK_FALSE(ConsoleCommands::parseLockRequest(args, error, "lock").has_value());
        CHECK(error == ConsoleCommands::lockUsage("lock"));
    }
    CHECK(ConsoleCommands::lockUsage("unlock").contains("unlock character <code>"));
    CHECK(ConsoleCommands::activateUsage().contains("activate switch#N"));
    // A token names one thing alone; a name every thing that answers to it, in any case.
    const std::vector<ConsoleCommands::NamedEntry> entries{{"switch#0", "BRIDGEPAD", "E1BRIDGE"},
                                                           {"switch#1", "BRIDGEPAD", "E1DOOR"},
                                                           {"switch#2", "TRIGGER2", "E1DOOR"}};
    CHECK(ConsoleCommands::entriesNamed(entries, "SWITCH#1") == std::vector<usize>{1});
    CHECK(ConsoleCommands::entriesNamed(entries, "bridgepad") == std::vector<usize>{0, 1});
    CHECK(ConsoleCommands::entriesNamed(entries, "e1door") == std::vector<usize>{1, 2});
    CHECK(ConsoleCommands::entriesNamed(entries, "trigger2") == std::vector<usize>{2});
    CHECK(ConsoleCommands::entriesNamed(entries, "switch#9").empty());
    CHECK(ConsoleCommands::entriesNamed(entries, "nothing").empty());
}

TEST_CASE("goto takes a map, a point, or a point in a map", "[dev-console][commands]") {
    std::string error;
    auto level = ConsoleCommands::parseGoto(Args{"g1"}, error);
    REQUIRE(level.has_value());
    CHECK(level->level == "G1");
    CHECK_FALSE(level->position.has_value());
    auto point = ConsoleCommands::parseGoto(Args{"10", "2.5", "-3"}, error);
    REQUIRE(point.has_value());
    CHECK_FALSE(point->level.has_value());
    REQUIRE(point->position.has_value());
    CHECK(point->position->x == Approx(10.0f));
    CHECK(point->position->y == Approx(2.5f));
    CHECK(point->position->z == Approx(-3.0f));
    auto both = ConsoleCommands::parseGoto(Args{"A1", "1", "2", "3"}, error);
    REQUIRE(both.has_value());
    CHECK(both->level == "A1");
    CHECK(both->position == Vec3{1, 2, 3});
    auto tower = ConsoleCommands::parseGoto(Args{"tower"}, error);
    REQUIRE(tower.has_value());
    CHECK(tower->level == "TOWER");
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{}, error).has_value());
    CHECK(error == ConsoleCommands::gotoUsage());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"a", "b"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"1", "2"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"G1", "1", "x", "3"}, error).has_value());
}

TEST_CASE("get reads the item, its amount and a potion's kind", "[dev-console][commands]") {
    std::string error;
    const auto get = [&](Args args) {
        const auto gift = ConsoleCommands::parseGet(args, error);
        REQUIRE(gift.has_value());
        return *gift;
    };
    auto key = get({"key"});
    CHECK(key.kind == Kind::Item);
    CHECK(key.offer.kind == static_cast<s32>(ItemKind::Keys));
    CHECK(key.offer.amount == ConsoleCommands::kDefaultKeys);
    CHECK(get({"KEYS", "3"}).offer.amount == 3);
    auto potion = get({"potion"});
    CHECK(potion.offer.kind == static_cast<s32>(ItemKind::Potion));
    CHECK(potion.offer.amount == 1);
    CHECK(potion.offer.flags == static_cast<u32>(ConsoleCommands::kDefaultPotionKind));
    auto acid = get({"potions", "2", "acid"});
    CHECK(acid.offer.amount == 2);
    CHECK(acid.offer.flags == 4);
    auto lightning = get({"potion", "lightning"});
    CHECK(lightning.offer.amount == 1);
    CHECK(lightning.offer.flags == 3);
    CHECK(get({"potion", "electric", "5"}).offer.flags == 2);
    auto gold = get({"gold"});
    CHECK(gold.offer.kind == static_cast<s32>(ItemKind::Gold));
    CHECK(gold.offer.amount == ConsoleCommands::kDefaultGold);
    auto health = get({"health", "50"});
    CHECK(health.offer.kind == static_cast<s32>(ItemKind::Food));
    CHECK(health.offer.amount == 50);
    auto wings = get({"levitate"});
    CHECK(wings.offer.kind == powerup::kSpecial);
    CHECK(wings.offer.flags == powerup::kLevitation);
    CHECK(wings.offer.strength == Approx(ConsoleCommands::kDefaultSeconds));
    CHECK(wings.name == "levitate");
    CHECK(get({"LEVITATE", "-1"}).offer.strength == Approx(-1.0f));
    CHECK(get({"invulnerable", "90"}).offer.strength == Approx(90.0f));
    auto shots = get({"supershot"});
    CHECK(shots.offer.kind == powerup::kWeapon);
    CHECK(shots.offer.flags == damage::kSuperShot);
    CHECK(shots.offer.amount > 0); // charges: one huge shot each
    CHECK(get({"supershot", "5"}).offer.amount == 5);
    auto halo = get({"antideath"});
    CHECK(halo.offer.kind == powerup::kArmor);
    CHECK(halo.offer.flags == 0x80000);
    auto exp = get({"exp", "500"});
    CHECK(exp.kind == Kind::Experience);
    CHECK(exp.amount == 500);
    CHECK(get({"experience", "7"}).amount == 7);
    auto level = get({"level", "42"});
    CHECK(level.kind == Kind::Level);
    CHECK(level.amount == 42);
    CHECK_FALSE(ConsoleCommands::parseGet(Args{}, error).has_value());
    CHECK(error == ConsoleCommands::getUsage());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"sword"}, error).has_value());
    CHECK(error.contains("sword"));
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"exp"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"exp", "-5"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"level", "0"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"level", "100"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"key", "x"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"key", "0"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"potion", "slime"}, error).has_value());
}

TEST_CASE("get says what came of the gift", "[dev-console][commands]") {
    std::string error;
    const auto keys = ConsoleCommands::parseGet(Args{"keys", "3"}, error);
    REQUIRE(keys.has_value());
    ItemTaking taking;
    taking.outcome = ItemTaking::Outcome::Taken;
    taking.count = 3;
    CHECK(ConsoleCommands::report(*keys, taking) == "Got 3 keys");
    taking.outcome = ItemTaking::Outcome::PartTaken;
    taking.count = 2;
    taking.left = 1;
    CHECK(ConsoleCommands::report(*keys, taking) == "Got 2 keys; no room for 1 more");
    taking.outcome = ItemTaking::Outcome::KeysFull;
    CHECK(ConsoleCommands::report(*keys, taking) == "No room for more keys");
    const auto wings = ConsoleCommands::parseGet(Args{"levitate", "-1"}, error);
    REQUIRE(wings.has_value());
    taking.outcome = ItemTaking::Outcome::Taken;
    CHECK(ConsoleCommands::report(*wings, taking) == "Got levitate for good");
    const auto timed = ConsoleCommands::parseGet(Args{"levitate", "45"}, error);
    REQUIRE(timed.has_value());
    CHECK(ConsoleCommands::report(*timed, taking) == "Got levitate for 45 seconds");
    taking.outcome = ItemTaking::Outcome::NotCarried;
    CHECK(ConsoleCommands::report(*wings, taking) == "Cannot carry levitate");
}

TEST_CASE("the item list names everything get knows", "[dev-console][commands]") {
    const auto lines = ConsoleCommands::itemLines();
    CHECK(lines.size() >= 30);
    for (const auto* const name : {"key", "potion", "gold", "health", "exp", "level", "levitate",
                                   "antideath", "supershot", "fiveway", "pojo", "fire, electric"}) {
        CHECK(mentions(lines, name));
    }
    std::string error;
    for (const std::string& line : lines) {
        // Every powerup line starts with a name get accepts.
        const std::string name = line.substr(0, line.find(' '));
        if (name == "exp" || name == "level") {
            CHECK(ConsoleCommands::parseGet(Args{name, "5"}, error).has_value());
        } else {
            CHECK(ConsoleCommands::parseGet(Args{name}, error).has_value());
        }
    }
}

TEST_CASE("held powerups and potions are named the way get names them", "[dev-console][commands]") {
    const PowerupSlot wings{
        .strength = 30.0f, .kind = powerup::kSpecial, .flags = powerup::kLevitation};
    CHECK(ConsoleCommands::powerupName(wings) == "levitate");
    const PowerupSlot giant{.strength = 30.0f,
                            .kind = powerup::kSpecial,
                            .flags = powerup::kGrowth | powerup::kEnemyShrink};
    CHECK(ConsoleCommands::powerupName(giant) == "growth+shrink");
    const PowerupSlot odd{.strength = 30.0f, .kind = powerup::kMagic, .flags = 0x12345};
    CHECK(ConsoleCommands::powerupName(odd).contains("kind 8"));
    CHECK(ConsoleCommands::potionName(0) == "fire");
    CHECK(ConsoleCommands::potionName(1) == "fire");
    CHECK(ConsoleCommands::potionName(2) == "electric");
    CHECK(ConsoleCommands::potionName(3) == "lightning");
    CHECK(ConsoleCommands::potionName(4) == "acid");
    CHECK(ConsoleCommands::potionName(9) == "fire");
}

TEST_CASE("the inventory lines tell what a character is, carries and has gathered",
          "[dev-console][commands]") {
    CharacterSave save;
    save.name = "AB";
    save.character = 1;
    save.gold = 55;
    ClassProgress& progress = save.progress();
    progress.experience = levelExperience(7);
    progress.health = 321;
    progress.inventory.keys = 2;
    progress.inventory.potions = {1, 4};
    progress.inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0.0f, 30.0f);
    progress.inventory.addPowerup(powerup::kWeapon, damage::kSuperShot, 4.0f, -1.0f);
    progress.relics.addRune(0);
    progress.relics.addRune(5);
    progress.relics.addShard(3);
    progress.relics.addLegend(2);
    progress.relics.gargoylePieces = {1, 2, 3};
    progress.crystals[3] = 4;
    const auto lines = ConsoleCommands::inventoryLines(save);
    for (const auto* const text :
         {"AB", "level 7", "321", "55", "Keys 2", "fire", "acid", "levitate", "30 s", "supershot",
          "for good", "x4", "2/13", "1/12", "2/20", "3/28"}) {
        CHECK(mentions(lines, text));
    }
    CharacterSave bare;
    bare.name = "CD";
    CHECK(mentions(ConsoleCommands::inventoryLines(bare), "No powerups"));
}

TEST_CASE("unlock_tower opens every wing and portal to the character", "[dev-console][commands]") {
    CharacterSave save;
    save.progress().relics.pendingRunes = 0x3;
    ConsoleCommands::unlockTower(save);
    const TowerAccess access(std::span<const CharacterSave>(&save, 1));
    for (s32 world = 1; world <= 11; ++world) {
        CHECK(access.worldOpen(world));
        for (s32 gate = 0; gate < LevelRecord::kLevelsPerRealm; ++gate) {
            CHECK(access.portalOpen(world, gate));
        }
    }
    CHECK(access.worldOpen(TowerAccess::kTowerWorld));
    CHECK(access.liftsOpen());
    const Relics& relics = save.progress().relics;
    CHECK(relics.runeCount() == Relics::kRuneCount);
    CHECK(relics.pendingRunes == 0);
    CHECK(relics.pendingShards == 0);
    CHECK(relics.legends == 0); // the bosses' items are left to be found
    // The gargoyle statues' wings open too: every kind's pieces are kept as collected and
    // congratulated, so no ceremony waits.
    for (s32 kind = 0; kind < static_cast<s32>(Relics::kGargoyleKinds); ++kind) {
        CHECK(relics.gargoyleComplete(kind));
        CHECK(relics.gargoylePieces[static_cast<usize>(kind)] < 0);
    }
    CHECK(relics.pendingCeremonies == 0);
}

TEST_CASE("the map list names every level of every realm", "[dev-console][commands][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto lines = ConsoleCommands::mapLines(catalog);
    CHECK(lines.size() == catalog.realms().size());
    CHECK(mentions(lines, "G1"));
    CHECK(mentions(lines, "A1"));
    for (const LevelCatalog::Realm& realm : catalog.realms()) {
        for (const std::string& name : realm.levels) {
            CHECK(mentions(lines, name));
        }
    }
}

TEST_CASE("list knows what a level holds and the map named after it", "[dev-console][commands]") {
    CHECK(ConsoleCommands::listingOf(Args{"enemies"}) == ConsoleCommands::Listing::Enemies);
    CHECK(ConsoleCommands::listingOf(Args{"Critters"}) == ConsoleCommands::Listing::Critters);
    CHECK(ConsoleCommands::listingOf(Args{"generators"}) == ConsoleCommands::Listing::Generators);
    CHECK(ConsoleCommands::listingOf(Args{"OBJECTS", "g1"}) == ConsoleCommands::Listing::Objects);
    CHECK(ConsoleCommands::listedMap(Args{"objects", "g1"}) == "G1");
    CHECK_FALSE(ConsoleCommands::listedMap(Args{"objects"}).has_value());
}

TEST_CASE("spawn reads an enemy with its tier and count, a critter, a generator or a thing",
          "[dev-console][commands]") {
    using Kind = ConsoleCommands::Spawn::Kind;
    std::string error;
    const auto spawn = [&](Args args) {
        const auto request = ConsoleCommands::parseSpawn(args, error);
        REQUIRE(request.has_value());
        return *request;
    };
    auto grunt = spawn({"gru"});
    CHECK(grunt.kind == Kind::Enemy);
    CHECK(grunt.enemyKind == kGruntKind);
    CHECK(grunt.tier == 1);
    CHECK(grunt.count == 1);
    CHECK(grunt.name == "GRU");
    auto crowd = spawn({"GRU", "3", "5"});
    CHECK(crowd.tier == 3);
    CHECK(crowd.count == 5);
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gru", "4"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gru", "1", "0"}, error).has_value());
    CHECK(spawn({"gru", "1", "2", "x"}).map == "X"); // whatever is left names a map
    auto golem = spawn({"golem"});
    CHECK(golem.kind == Kind::Critter);
    CHECK(golem.critter == CombatantKind::Golem);
    CHECK(golem.form.empty());
    CHECK(spawn({"gargoyle"}).form == "GAR_EAGL");
    CHECK(spawn({"Gargoyle", "lion"}).form == "GAR_LION");
    CHECK(spawn({"general"}).critter == CombatantKind::General);
    CHECK(spawn({"gargoyle", "dog"}).map == "DOG");
    CHECK(spawn({"general", "x"}).map == "X");
    auto generator = spawn({"generator", "rat", "2"});
    CHECK(generator.kind == Kind::Generator);
    CHECK(generator.enemyKind == kRatKind);
    CHECK(generator.tier == 2);
    CHECK(generator.name == "RAT");
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"generator"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"generator", "golem"}, error).has_value());
    auto key = spawn({"key"});
    CHECK(key.kind == Kind::Thing);
    CHECK(key.name == "KEY");
    CHECK(spawn({"key", "x"}).map == "X");
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{}, error).has_value());
    CHECK(error == ConsoleCommands::spawnUsage());
    CHECK(ConsoleCommands::spawnableEnemy(kGruntKind));
    CHECK(ConsoleCommands::spawnableEnemy(kDeathKind));
    CHECK(ConsoleCommands::spawnableEnemy(kItKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kGolemEnemyKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kGargoyleEnemyKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kGeneralEnemyKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kEnemyKindCount));
}

TEST_CASE("the enemy, critter and roster lines name what spawn takes", "[dev-console][commands]") {
    const auto enemies = ConsoleCommands::enemyKindLines();
    REQUIRE(enemies.size() == 1);
    CHECK(enemies[0].contains("GRU"));
    CHECK(enemies[0].contains("DEATH"));
    CHECK_FALSE(enemies[0].contains("GOLEM"));
    CHECK_FALSE(enemies[0].contains("NONE"));
    const auto critters = ConsoleCommands::critterLines();
    REQUIRE(critters.size() == 1);
    CHECK(critters[0].contains("golem"));
    CHECK(critters[0].contains("eagl|lion|serp"));
    const std::vector<LevelEnemy> roster{LevelEnemy{.kind = kGruntKind, .subtype = 2},
                                         LevelEnemy{.kind = kRatKind, .subtype = 1}};
    const auto lines = ConsoleCommands::rosterLines(roster);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("medium GRU"));
    CHECK(lines[0].contains("small RAT"));
    CHECK(ConsoleCommands::rosterLines({})[0] == "Roster: none");
    CHECK(ConsoleCommands::pickupKindName(2) == "keys");
    CHECK(ConsoleCommands::pickupKindName(99) == "pickup");
}

TEST_CASE("the layout lines count a level's generators, placed enemies, pickups and objects",
          "[dev-console][commands][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    const auto generators = ConsoleCommands::generatorLines(layout);
    REQUIRE(generators.size() == 1);
    CHECK(generators[0].contains("GRU"));
    CHECK(generators[0].contains("tier"));
    CHECK(ConsoleCommands::placedEnemyLines(layout)[0].starts_with("Placed enemies: "));
    CHECK(ConsoleCommands::placedCritterLines(layout)[0].starts_with("Critters placed: "));
    const auto pickups = ConsoleCommands::pickupLines(layout);
    REQUIRE(pickups.size() == 1);
    CHECK(pickups[0].starts_with("Pickups (spawn <name>): "));
    CHECK(pickups[0].contains("(keys)"));
    const auto objects = ConsoleCommands::objectLines(layout);
    REQUIRE(objects.size() == 1);
    CHECK(objects[0].starts_with(std::format("Objects ({}, ", layout.objects().size())));
    // The switches, in the order the level binds them, each with its token and target.
    const auto switches = ConsoleCommands::switchesOf(layout);
    REQUIRE_FALSE(switches.empty());
    for (usize i = 0; i < switches.size(); ++i) {
        CHECK(switches[i].index == static_cast<s32>(i));
        CHECK_FALSE(switches[i].name.empty());
    }
    CHECK(std::ranges::any_of(switches, [](const auto& entry) { return !entry.target.empty(); }));
    const auto switchLines = ConsoleCommands::switchLines(switches);
    REQUIRE(switchLines.size() == switches.size() + 1);
    CHECK(switchLines[0].starts_with(std::format("Switches ({}; activate ", switches.size())));
    CHECK(switchLines[1].starts_with("switch#0 "));
    CHECK(switchLines[1].contains(" id "));
    const auto named = ConsoleCommands::switchEntries(switches);
    REQUIRE(named.size() == switches.size());
    CHECK(named[0].token == "switch#0");
    CHECK(ConsoleCommands::entriesNamed(named, "switch#0") == std::vector<usize>{0});
    CHECK(ConsoleCommands::switchLines({}) == std::vector<std::string>{"Switches: none"});
    CHECK(ConsoleCommands::instanceName(layout, -1).empty());
    CHECK(objects[0].contains(" distinct): "));
    REQUIRE_FALSE(layout.objects().empty());
    std::string lowered = layout.objects().front().name;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(ConsoleCommands::hasObject(layout, lowered));
    CHECK_FALSE(ConsoleCommands::hasObject(layout, "NOTHING_OF_THE_KIND"));
    const auto pickup = std::ranges::find_if(
        layout.itemInfos(), [](const ItemInfo& info) { return info.type == ItemInfo::kPowerup; });
    REQUIRE(pickup != layout.itemInfos().end());
    CHECK(ConsoleCommands::hasPickup(layout, pickup->name));
    CHECK_FALSE(ConsoleCommands::hasPickup(layout, "NOTHING_OF_THE_KIND"));
}

TEST_CASE("spawn takes the map a visitor comes from after everything else",
          "[dev-console][commands]") {
    using Kind = ConsoleCommands::Spawn::Kind;
    std::string error;
    const auto spawn = [&](Args args) {
        const auto request = ConsoleCommands::parseSpawn(args, error);
        REQUIRE(request.has_value());
        return *request;
    };
    auto visitors = spawn({"gru", "2", "3", "e2"});
    CHECK(visitors.kind == Kind::Enemy);
    CHECK(visitors.tier == 2);
    CHECK(visitors.count == 3);
    CHECK(visitors.map == "E2");
    auto one = spawn({"gru", "e2"});
    CHECK(one.tier == 1);
    CHECK(one.count == 1);
    CHECK(one.map == "E2");
    CHECK_FALSE(spawn({"gru", "2"}).map.has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gru", "1", "2", "e2", "x"}, error).has_value());
    auto generator = spawn({"generator", "war", "1", "E2"});
    CHECK(generator.kind == Kind::Generator);
    CHECK(generator.tier == 1);
    CHECK(generator.map == "E2");
    CHECK(spawn({"generator", "war", "e2"}).map == "E2");
    auto lion = spawn({"gargoyle", "lion", "d1"});
    CHECK(lion.form == "GAR_LION");
    CHECK(lion.map == "D1");
    CHECK(spawn({"gargoyle", "d1"}).form == "GAR_EAGL");
    CHECK(spawn({"gargoyle", "d1"}).map == "D1");
    CHECK(spawn({"golem", "f1"}).map == "F1");
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"golem", "f1", "x"}, error).has_value());
    auto lamp = spawn({"L1XS_LAMPHO#12", "l1"});
    CHECK(lamp.kind == Kind::Thing);
    CHECK(lamp.name == "L1XS_LAMPHO#12");
    CHECK(lamp.map == "L1");
    CHECK_FALSE(spawn({"key"}).map.has_value());
}

TEST_CASE("spawn takes a category word first and a map before an object",
          "[dev-console][commands]") {
    using Kind = ConsoleCommands::Spawn::Kind;
    std::string error;
    const auto spawn = [&](Args args) {
        const auto request = ConsoleCommands::parseSpawn(args, error);
        REQUIRE(request.has_value());
        return *request;
    };
    auto candle = spawn({"object", "E1XLNSNC_CNDL01", "e1"});
    CHECK(candle.kind == Kind::Object);
    CHECK(candle.name == "E1XLNSNC_CNDL01");
    CHECK(candle.map == "E1");
    auto mapFirst = spawn({"object", "e1", "E1XLNSNC_CNDL01"});
    CHECK(mapFirst.kind == Kind::Object);
    CHECK(mapFirst.name == "E1XLNSNC_CNDL01");
    CHECK(mapFirst.map == "E1");
    auto here = spawn({"object", "E1XLNSNC_CNDL01"});
    CHECK(here.kind == Kind::Object);
    CHECK_FALSE(here.map.has_value());
    auto key = spawn({"item", "key"});
    CHECK(key.kind == Kind::Item);
    CHECK(key.name == "KEY");
    CHECK(spawn({"pickup", "key"}).kind == Kind::Item);
    auto grunts = spawn({"enemy", "gru", "2", "4", "a1"});
    CHECK(grunts.kind == Kind::Enemy);
    CHECK(grunts.enemyKind == kGruntKind);
    CHECK(grunts.tier == 2);
    CHECK(grunts.count == 4);
    CHECK(grunts.map == "A1");
    auto serpent = spawn({"critter", "gargoyle", "serp", "d1"});
    CHECK(serpent.kind == Kind::Critter);
    CHECK(serpent.form == "GAR_SERP");
    CHECK(serpent.map == "D1");
    CHECK(spawn({"generator", "rat"}).kind == Kind::Generator);
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"enemy", "golem"}, error).has_value());
    CHECK(error.contains("No enemy kind named GOLEM"));
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"critter", "gru"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"object"}, error).has_value());
    CHECK(error == ConsoleCommands::spawnUsage());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"object", "a", "b", "c"}, error).has_value());
    CHECK(ConsoleCommands::looksLikeMap("e2"));
    CHECK(ConsoleCommands::looksLikeMap("S10"));
    CHECK(ConsoleCommands::looksLikeMap("tower"));
    CHECK_FALSE(ConsoleCommands::looksLikeMap("E1XLNSNC_CNDL01"));
    CHECK_FALSE(ConsoleCommands::looksLikeMap("gru"));
    CHECK_FALSE(ConsoleCommands::looksLikeMap("1"));
}

TEST_CASE("usage lines put the forms in a column with what each does beside them",
          "[dev-console][commands]") {
    for (const std::string& text : {ConsoleCommands::spawnUsage(), ConsoleCommands::getUsage(),
                                    ConsoleCommands::listUsage(), ConsoleCommands::gotoUsage()}) {
        CAPTURE(text);
        std::vector<std::string> lines;
        for (usize start = 0; start <= text.size();) {
            const usize end = std::min(text.find('\n', start), text.size());
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        REQUIRE(lines.size() >= 2);
        // Every form line's description starts at the same column, past the padding.
        const auto descriptionColumn = [](const std::string& line) {
            const usize gap = line.find("  ");
            return gap == std::string::npos ? std::string::npos : line.find_first_not_of(' ', gap);
        };
        const usize column = descriptionColumn(lines[0]);
        REQUIRE(column != std::string::npos);
        const std::string command = lines[0].substr(0, lines[0].find(' '));
        for (const std::string& line : lines) {
            if (line.starts_with(command)) {
                CHECK(descriptionColumn(line) == column);
                CHECK(line.size() > column);
            }
        }
    }
    CHECK(ConsoleCommands::spawnUsage().contains("spawn object <name> [map]"));
    CHECK(ConsoleCommands::spawnUsage().contains("spawn item <name>"));
    CHECK(ConsoleCommands::listUsage().contains("list objects [map]"));
}

TEST_CASE("spawn generator special asks for a realm's mixed brood", "[dev-console][commands]") {
    std::string error;
    const auto brood = ConsoleCommands::parseSpawn(Args{"generator", "special", "e2"}, error);
    REQUIRE(brood.has_value());
    CHECK(brood->kind == ConsoleCommands::Spawn::Kind::Generator);
    CHECK(brood->brood);
    CHECK(brood->enemyKind == -1);
    CHECK(brood->name == "SPECIAL");
    CHECK(brood->map == "E2");
    CHECK(brood->tier == 1);
    const auto tough = ConsoleCommands::parseSpawn(Args{"generator", "SPECIAL", "3"}, error);
    REQUIRE(tough.has_value());
    CHECK(tough->brood);
    CHECK(tough->tier == 3);
    CHECK_FALSE(tough->map.has_value());
    const auto grunts = ConsoleCommands::parseSpawn(Args{"generator", "gru"}, error);
    REQUIRE(grunts.has_value());
    CHECK_FALSE(grunts->brood);
    CHECK(ConsoleCommands::spawnUsage().contains("<enemy>|special"));
}

TEST_CASE("a layout's generator record is found by name", "[dev-console][commands][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    const ItemInfo* grunts = ConsoleCommands::generatorRecord(layout, "gru");
    REQUIRE(grunts != nullptr);
    CHECK(grunts->type == ItemInfo::kGenerator);
    CHECK(grunts->name == "GRU");
    CHECK(ConsoleCommands::generatorRecord(layout, "NOTHING_OF_THE_KIND") == nullptr);
}

TEST_CASE("get info asks after the thing selected, or one named by its token",
          "[dev-console][commands]") {
    std::string error;
    const auto selected = ConsoleCommands::parseGet(Args{"info"}, error);
    REQUIRE(selected.has_value());
    CHECK(selected->kind == Kind::Info);
    CHECK(selected->name.empty());
    const auto named = ConsoleCommands::parseGet(Args{"INFO", "object#3"}, error);
    REQUIRE(named.has_value());
    CHECK(named->kind == Kind::Info);
    CHECK(named->name == "object#3");
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"info", "object#3", "more"}, error).has_value());
    CHECK(error.find("get info") != std::string::npos);
    CHECK(ConsoleCommands::getUsage().find("get info [$SELECTED]") != std::string::npos);
}

TEST_CASE("goto takes a Legends map by name or platform, a preview of it, and the way back",
          "[dev-console][commands][legends]") {
    std::string error;
    auto named = ConsoleCommands::parseGoto(Args{"n64", "Gauntlet"}, error);
    REQUIRE(named.has_value());
    CHECK(named->legends == "Gauntlet"); // the case kept: it names a folder
    CHECK(named->platform == LegendsPlatform::N64);
    CHECK_FALSE(named->preview);
    CHECK_FALSE(named->back);
    CHECK_FALSE(named->level.has_value());
    CHECK_FALSE(named->position.has_value());
    auto preview = ConsoleCommands::parseGoto(Args{"DC", "castle", "Preview"}, error);
    REQUIRE(preview.has_value());
    CHECK(preview->legends == "castle");
    CHECK(preview->platform == LegendsPlatform::Dreamcast);
    CHECK(preview->preview);
    auto bare = ConsoleCommands::parseGoto(Args{"castle"}, error);
    REQUIRE(bare.has_value());
    CHECK(bare->legends == "castle");
    CHECK_FALSE(bare->platform.has_value());
    CHECK_FALSE(bare->level.has_value());
    auto barePreview = ConsoleCommands::parseGoto(Args{"castle", "preview"}, error);
    REQUIRE(barePreview.has_value());
    CHECK(barePreview->legends == "castle");
    CHECK(barePreview->preview);
    auto back = ConsoleCommands::parseGoto(Args{"back"}, error);
    REQUIRE(back.has_value());
    CHECK(back->back);
    CHECK_FALSE(back->legends.has_value());
    // A level's name is still a level; a platform alone, or with a point, is nothing.
    CHECK(ConsoleCommands::parseGoto(Args{"e2"}, error)->level == "E2");
    CHECK(ConsoleCommands::parseGoto(Args{"Tower"}, error)->level == "TOWER");
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"n64"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"n64", "back"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"n64", "1", "2", "3"}, error).has_value());
    CHECK(error == ConsoleCommands::gotoUsage());
    CHECK(ConsoleCommands::gotoUsage().contains("legends"));
    CHECK(ConsoleCommands::platformOf("Dreamcast") == LegendsPlatform::Dreamcast);
    CHECK(ConsoleCommands::platformOf("N64") == LegendsPlatform::N64);
    CHECK_FALSE(ConsoleCommands::platformOf("gamecube").has_value());
    CHECK(ConsoleCommands::platformName(LegendsPlatform::N64) == "N64");
}

TEST_CASE("goto's Legends forms never reach the shared game; the rest name a level or a point",
          "[dev-console][commands][online-console]") {
    std::string error;
    for (const Args& words : {Args{"back"}, Args{"return"}, Args{"n64", "castle"},
                              Args{"castle", "preview"}, Args{"MISSING"}, Args{"lab"}}) {
        CAPTURE(words);
        const auto destination = ConsoleCommands::parseGoto(words, error);
        REQUIRE(destination.has_value());
        CHECK(destination->offlineOnly());
    }
    for (const Args& words :
         {Args{"g1"}, Args{"tower"}, Args{"10", "2.5", "-3"}, Args{"A1", "1", "2", "3"}}) {
        CAPTURE(words);
        const auto destination = ConsoleCommands::parseGoto(words, error);
        REQUIRE(destination.has_value());
        CHECK_FALSE(destination->offlineOnly());
        CHECK((destination->level || destination->position));
    }
}

TEST_CASE("list maps names the Legends maps installed on a line per platform",
          "[dev-console][commands][legends]") {
    CHECK(ConsoleCommands::legendsMapLines(LegendsPlatform::N64, {}).empty());
    const std::vector<std::string> names{"castle", "forest"};
    const auto lines = ConsoleCommands::legendsMapLines(LegendsPlatform::Dreamcast, names);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("Dreamcast"));
    CHECK(lines[0].contains("castle, forest"));
    CHECK(lines[0].contains("goto"));
}

TEST_CASE("party_count takes a count from one to four, or off", "[dev-console][commands]") {
    std::string error;
    const auto three = ConsoleCommands::parsePartyCount(Args{"3"}, error);
    REQUIRE(three.has_value());
    CHECK(*three == 3);
    const auto off = ConsoleCommands::parsePartyCount(Args{"OFF"}, error);
    REQUIRE(off.has_value());
    CHECK_FALSE(off->has_value());
    for (const Args& args : {Args{}, Args{"0"}, Args{"5"}, Args{"two"}, Args{"2", "3"}}) {
        CAPTURE(args.size());
        CHECK_FALSE(ConsoleCommands::parsePartyCount(args, error).has_value());
        CHECK(error == ConsoleCommands::partyCountUsage());
    }
}

} // namespace
