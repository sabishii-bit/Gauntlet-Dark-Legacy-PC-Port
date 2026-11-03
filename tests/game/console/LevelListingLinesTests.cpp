#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/console/FixtureRequest.h"
#include "game/console/GiftRequest.h"
#include "game/console/LevelListingLines.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/InstanceNames.h"
#include "game/level/LevelCatalog.h"
#include "game/level/SyntheticLayout.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Args = std::vector<std::string>;

bool mentions(std::span<const std::string> lines, std::string_view text) {
    return std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); });
}

/** What list answers words it cannot read. */
constexpr std::string_view kListUsage =
    "list maps              every level, by realm\n"
    "list items [map]       what get knows, then a level's pickups\n"
    "list inventory         what your character is and carries\n"
    "list enemies [map]     the swarm's kinds, then a level's roster and generators\n"
    "list critters [map]    the great ones, then those a level places\n"
    "list generators [map]  a level's generators\n"
    "list objects [map]     a level's objects\n"
    "list switches [map]    a level's switches, for activate\n"
    "list characters        every playable character, locked or hidden too";

/** What list items says, a line an item. */
constexpr std::array<std::string_view, 34> kItemLines{
    "key [n]            keys for the doors (1)",
    "potion [n] [kind]  potions: fire, electric, lightning or acid (1 fire)",
    "gold [n]           gold for the shop (100)",
    "health [n]         health, as food gives it (100)",
    "exp <points>       experience to add",
    "level <level>      the level to be, 1 to 99",
    "levitate [s]       held above the ground (60 s; under nought for good)",
    "invulnerable [s]   takes no harm (60 s; under nought for good)",
    "goldinvulnerable [s] takes no harm, gilded (60 s; under nought for good)",
    "antideath [s]      a halo Death cannot take (60 s; under nought for good)",
    "reflectshield [s]  turns missiles back (60 s; under nought for good)",
    "fireshield [s]     burns what it is against (60 s; under nought for good)",
    "lightningshield [s] shocks what it is against (60 s; under nought for good)",
    "supershot [n]      one huge shot a charge (10 charges)",
    "threeway [s]       three shots at once (60 s; under nought for good)",
    "fiveway [s]        five shots at once (60 s; under nought for good)",
    "rapidfire [s]      shoots faster (60 s; under nought for good)",
    "reflect [s]        shots that bounce off the walls (60 s; under nought for good)",
    "thunderhammer [n]  a hammer blow of lightning a charge (10 charges)",
    "firebreath [n]     breathes fire a charge (10 charges)",
    "acidbreath [n]     breathes acid a charge (10 charges)",
    "lightningbreath [n] breathes lightning a charge (10 charges)",
    "phoenix [s]        a fire-spitting companion (60 s; under nought for good)",
    "growth [s]         grows the character (60 s; under nought for good)",
    "shrink [s]         shrinks the swarm (60 s; under nought for good)",
    "invisible [s]      unseen by the enemies (60 s; under nought for good)",
    "xray [s]           sees inside the nearest closed chest (60 s; under nought for good)",
    "stoptime [s]       halts ordinary opponents and hazards (60 s; under nought for good)",
    "turbo [s]          fills the turbo meter at once (60 s; under nought for good)",
    "speed [s]          runs faster (60 s; under nought for good)",
    "mikey [s]          a decoy the swarm chases (60 s; under nought for good)",
    "handofdeath [s]    returns melee harm to the striker (60 s; under nought for good)",
    "healthvamp [s]     melee harm heals the character (60 s; under nought for good)",
    "pojo [s]           Pojo speaks for the character (60 s; under nought for good)",
};

TEST_CASE("list knows maps, items and inventory in any case", "[dev-console][commands]") {
    CHECK(listingOf(Args{"maps"}) == Listing::Maps);
    CHECK(listingOf(Args{"ITEMS"}) == Listing::Items);
    CHECK(listingOf(Args{"Inventory"}) == Listing::Inventory);
    CHECK(listingOf(Args{}) == Listing::Unknown);
    CHECK(listingOf(Args{"shops"}) == Listing::Unknown);
    CHECK(listingOf(Args{"Switches"}) == Listing::Switches);
    CHECK(listingOf(Args{"characters"}) == Listing::Characters);
    CHECK(listUsage().contains("list switches"));
    CHECK(listUsage().contains("list characters"));
}

TEST_CASE("list characters names every class with its standing and the name costumes",
          "[dev-console][commands][characters]") {
    const auto lines = characterLines(0);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "Starting: WAR, VAL, WIZ, ARC, DWF, KNI, SOR, JES");
    CHECK(lines[1].starts_with("Unlockable (unlock character <code>): MIN locked, "));
    CHECK(lines[1].ends_with("SUM hidden"));
    CHECK(lines[2].contains("CYBMOS (WAR HIR)"));
    const auto unlocked = characterLines(0x101);
    CHECK(unlocked[1].contains("MIN unlocked"));
    CHECK(unlocked[1].contains("FAL locked"));
    CHECK(unlocked[1].ends_with("SUM unlocked"));
}

TEST_CASE("the item list names everything get knows", "[dev-console][commands]") {
    const auto lines = itemLines();
    CHECK(lines.size() >= 30);
    for (const auto* const name : {"key", "potion", "gold", "health", "exp", "level", "levitate",
                                   "antideath", "supershot", "fiveway", "pojo", "fire, electric"}) {
        CHECK(mentions(lines, name));
    }
    for (const std::string& line : lines) {
        // Every powerup line starts with a name get accepts.
        const std::string name = line.substr(0, line.find(' '));
        if (name == "exp" || name == "level") {
            CHECK(parseGift(Args{name, "5"}).has_value());
        } else {
            CHECK(parseGift(Args{name}).has_value());
        }
    }
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
    const auto lines = inventoryLines(save);
    for (const auto* const text :
         {"AB", "level 7", "321", "55", "Keys 2", "fire", "acid", "levitate", "30 s", "supershot",
          "for good", "x4", "2/13", "1/12", "2/20", "3/28"}) {
        CHECK(mentions(lines, text));
    }
    CharacterSave bare;
    bare.name = "CD";
    CHECK(mentions(inventoryLines(bare), "No powerups"));
}

TEST_CASE("the map list names every level of every realm", "[dev-console][commands][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto lines = mapLines(catalog);
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
    CHECK(listingOf(Args{"enemies"}) == Listing::Enemies);
    CHECK(listingOf(Args{"Critters"}) == Listing::Critters);
    CHECK(listingOf(Args{"generators"}) == Listing::Generators);
    CHECK(listingOf(Args{"OBJECTS", "g1"}) == Listing::Objects);
    CHECK(listedMap(Args{"objects", "g1"}) == "G1");
    CHECK_FALSE(listedMap(Args{"objects"}).has_value());
}

TEST_CASE("the enemy, critter and roster lines name what spawn takes", "[dev-console][commands]") {
    const auto enemies = enemyKindLines();
    REQUIRE(enemies.size() == 1);
    CHECK(enemies[0].contains("GRU"));
    CHECK(enemies[0].contains("DEATH"));
    CHECK_FALSE(enemies[0].contains("GOLEM"));
    CHECK_FALSE(enemies[0].contains("NONE"));
    const auto critters = critterLines();
    REQUIRE(critters.size() == 1);
    CHECK(critters[0].contains("golem"));
    CHECK(critters[0].contains("eagl|lion|serp"));
    const std::vector<LevelEnemy> roster{LevelEnemy{.kind = kGruntKind, .subtype = 2},
                                         LevelEnemy{.kind = kRatKind, .subtype = 1}};
    const auto lines = rosterLines(roster);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("medium GRU"));
    CHECK(lines[0].contains("small RAT"));
    CHECK(rosterLines({})[0] == "Roster: none");
}

TEST_CASE("the layout lines count a level's generators, placed enemies, pickups and objects",
          "[dev-console][commands][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    const auto generators = generatorLines(layout);
    REQUIRE(generators.size() == 1);
    CHECK(generators[0].contains("GRU"));
    CHECK(generators[0].contains("tier"));
    CHECK(placedEnemyLines(layout)[0].starts_with("Placed enemies: "));
    CHECK(placedCritterLines(layout)[0].starts_with("Critters placed: "));
    const auto pickups = pickupLines(layout);
    REQUIRE(pickups.size() == 1);
    CHECK(pickups[0].starts_with("Pickups (spawn <name>): "));
    CHECK(pickups[0].contains("(keys)"));
    const auto objects = objectLines(layout);
    REQUIRE(objects.size() == 1);
    CHECK(objects[0].starts_with(std::format("Objects ({}, ", layout.objects().size())));
    // The switches, in the order the level binds them, each with its token and target.
    const auto switches = switchesOf(layout);
    REQUIRE_FALSE(switches.empty());
    for (usize i = 0; i < switches.size(); ++i) {
        CHECK(switches[i].index == static_cast<s32>(i));
        CHECK_FALSE(switches[i].name.empty());
    }
    CHECK(std::ranges::any_of(switches, [](const auto& entry) { return !entry.target.empty(); }));
    const auto lines = switchLines(switches);
    REQUIRE(lines.size() == switches.size() + 1);
    CHECK(lines[0].starts_with(std::format("Switches ({}; activate ", switches.size())));
    CHECK(lines[1].starts_with("switch#0 "));
    CHECK(lines[1].contains(" id "));
    const auto named = switchEntries(switches);
    REQUIRE(named.size() == switches.size());
    CHECK(named[0].token == "switch#0");
    CHECK(entriesNamed(named, "switch#0") == std::vector<usize>{0});
    CHECK(switchLines({}) == std::vector<std::string>{"Switches: none"});
    CHECK(instanceName(layout, -1).empty());
    CHECK(objects[0].contains(" distinct): "));
}

TEST_CASE("list's usage reads as it does today", "[dev-console][commands]") {
    CHECK(listUsage() == kListUsage);
}

TEST_CASE("the item, kind and character listings read as they do today",
          "[dev-console][commands]") {
    CHECK(itemLines() == std::vector<std::string>(kItemLines.begin(), kItemLines.end()));
    CHECK(
        enemyKindLines() ==
        std::vector<std::string>{
            "Enemies (spawn <enemy> [tier 1-3] [count]): SCO, TRO, DEM, RAT, GRU, KNI, SNA, SOR, "
            "MUM, SPI, LIZ, TRE, MAG, ZOM, PLA, WOL, ICE, WRM, DOG, SKE, GHO, ACI, HAN, IMP, WAR, "
            "SKY, WIND, GRM, DEATH, IT"});
    CHECK(critterLines() ==
          std::vector<std::string>{
              "Critters (spawn <critter>): golem, general, gargoyle [eagl|lion|serp]"});
    CHECK(characterLines(0) ==
          std::vector<std::string>{
              "Starting: WAR, VAL, WIZ, ARC, DWF, KNI, SOR, JES",
              "Unlockable (unlock character <code>): MIN locked, FAL locked, JAC locked, TIG "
              "locked, OGR locked, UNI locked, MED locked, HYE locked, SUM hidden",
              "Costumes by name, entered at the select screen: ICE600 (DWF GEI), NUD069 (DWF SNM), "
              "STX222 (JES STK), KJH105 (JES KJH), PNK666 (JES PNK), BAT900 (KNI GEB), TAK118 (KNI "
              "NIN), STG333 (KNI STG), KAO292 (KNI WTR), CSS222 (KNI CSS), RIZ721 (KNI RIZ), "
              "ARV984 (KNI ARV), DIB626 (KNI DIB), SJB964 (KNI SJB), TWN300 (VAL GET), AYA555 (VAL "
              "SCH), CEL721 (VAL CEL), CAS400 (WAR GEC), MTN200 (WAR GEM), RAT333 (WAR RAT), "
              "GARM99 (WIZ GA2), GARM00 (WIZ GAM), DES700 (WIZ GED), SKY100 (WIZ GEP), SUM224 (WIZ "
              "SUM), DARTHC (KNI DCY), CYBMOS (WAR HIR)"});
}

TEST_CASE("the inventory and roster lines read as they do today", "[dev-console][commands]") {
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
    progress.inventory.powerups[0].on = false;
    progress.relics.addRune(0);
    progress.relics.addRune(5);
    progress.relics.addShard(3);
    progress.relics.addLegend(2);
    progress.relics.gargoylePieces = {1, 2, 3};
    progress.crystals[3] = 4;
    progress.crystals[7] = 12;
    CHECK(inventoryLines(save) ==
          std::vector<std::string>{
              "AB the VAL at level 7 (7260 experience), health 321/1100, 55 gold",
              "Keys 2/9, potions 2/9: fire, acid",
              "Powerups: levitate 30 s (off), supershot for good x4",
              "Runestones 2/13, boss shards 1, legend items 1, gargoyle pieces 1/12 2/20 3/28",
              "Crystals: gate 3: 4, gate 7: 12"});
    CharacterSave bare;
    bare.name = "CD";
    CHECK(inventoryLines(bare) ==
          std::vector<std::string>{
              "CD the WAR at level 1 (0 experience), health 500/500, 0 gold",
              "Keys 0/9, potions 0/9", "No powerups",
              "Runestones 0/13, boss shards 0, legend items 0, gargoyle pieces 0/12 0/20 0/28",
              "Crystals: none"});
    const std::vector<LevelEnemy> roster{
        LevelEnemy{.kind = kGruntKind, .subtype = 2}, LevelEnemy{.kind = kRatKind, .subtype = 1},
        LevelEnemy{.kind = kGruntKind, .subtype = 4}, LevelEnemy{.kind = kRatKind, .subtype = 0},
        LevelEnemy{.kind = -1, .subtype = 1}};
    CHECK(rosterLines(roster) ==
          std::vector<std::string>{"Roster: medium GRU, small RAT, medium other GRU, RAT"});
    CHECK(rosterLines({}) == std::vector<std::string>{"Roster: none"});
}

TEST_CASE("a level's lines read the same without the disc", "[dev-console][commands]") {
    const WorldLayout layout = test::syntheticLayout();
    auto switches = switchesOf(layout);
    REQUIRE(switches.size() == 3);
    CHECK(switchLines(switches) ==
          std::vector<std::string>{"Switches (3; activate <switch#N|id|name>):",
                                   "switch#0 BRIDGEPAD id 10 -> E1DOOR, waiting",
                                   "switch#1 LIFTPAD id 11 -> E1BRIDGE, fired through another, "
                                   "waiting",
                                   "switch#2 TARGET id 12, shootable, waiting"});
    switches[1].fired = true;
    switches[2].enabled = false;
    CHECK(
        switchLines(switches) ==
        std::vector<std::string>{"Switches (3; activate <switch#N|id|name>):",
                                 "switch#0 BRIDGEPAD id 10 -> E1DOOR, waiting",
                                 "switch#1 LIFTPAD id 11 -> E1BRIDGE, fired through another, fired",
                                 "switch#2 TARGET id 12, shootable, not for this party, waiting"});
    CHECK(pickupLines(layout) ==
          std::vector<std::string>{
              "Pickups (spawn <name>): KEY (keys), TREASURE (gold), ODDITY (pickup)"});
    CHECK(objectLines(layout) ==
          std::vector<std::string>{"Objects (5, 4 distinct): CANDLE x2, E1BRIDGE, E1DOOR, GROUND"});
    CHECK(generatorLines(layout) ==
          std::vector<std::string>{"Generators: GRU x2 (tier 1, 3); RAT (tier 2)"});
    CHECK(placedEnemyLines(layout) == std::vector<std::string>{"Placed enemies: GRU x2, RAT"});
    CHECK(placedCritterLines(layout) ==
          std::vector<std::string>{"Critters placed: GAR_EAGL, GOLEM"});
}

} // namespace
