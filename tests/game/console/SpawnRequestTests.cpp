#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/console/SpawnRequest.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/SyntheticLayout.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Args = std::vector<std::string>;
using Kind = SpawnRequest::Kind;

/** What spawn answers words it cannot read. */
constexpr std::string_view kSpawnUsage =
    "spawn <enemy> [tier] [count] [map]            so many of the swarm's kind, at a tier\n"
    "spawn generator <enemy>|special [tier] [map]  a generator breeding that kind, or the "
    "temple's or underworld's mixed brood\n"
    "spawn golem|general|gargoyle [form] [map]     a great one; a gargoyle's form is eagl, lion "
    "or serp\n"
    "spawn item <name>                             a pickup of this level, by its record\n"
    "spawn object <name> [map]                     a copy of a level's object\n"
    "A map (E2) brings the kind's voices, a great one's costume or the object from that level; "
    "list enemies, critters, items and objects name what there is.";

/** Words for spawn and what reading them says. */
struct Refusal {
    Args words;
    std::string_view error;
};

/** What spawn reads of `args`, which it must be able to read. */
SpawnRequest spawn(const Args& args) {
    const auto request = parseSpawn(args);
    REQUIRE(request.has_value());
    return *request;
}

TEST_CASE("spawn reads an enemy with its tier and count, a critter, a generator or a thing",
          "[dev-console][commands]") {
    auto grunt = spawn({"gru"});
    CHECK(grunt.kind == Kind::Enemy);
    CHECK(grunt.enemyKind == kGruntKind);
    CHECK(grunt.tier == 1);
    CHECK(grunt.count == 1);
    CHECK(grunt.name == "GRU");
    auto crowd = spawn({"GRU", "3", "5"});
    CHECK(crowd.tier == 3);
    CHECK(crowd.count == 5);
    CHECK_FALSE(parseSpawn(Args{"gru", "4"}).has_value());
    CHECK_FALSE(parseSpawn(Args{"gru", "1", "0"}).has_value());
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
    CHECK_FALSE(parseSpawn(Args{"generator"}).has_value());
    CHECK_FALSE(parseSpawn(Args{"generator", "golem"}).has_value());
    auto key = spawn({"key"});
    CHECK(key.kind == Kind::Thing);
    CHECK(key.name == "KEY");
    CHECK(spawn({"key", "x"}).map == "X");
    const auto none = parseSpawn(Args{});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error() == spawnUsage());
    CHECK(spawnableEnemy(kGruntKind));
    CHECK(spawnableEnemy(kDeathKind));
    CHECK(spawnableEnemy(kItKind));
    CHECK_FALSE(spawnableEnemy(kGolemEnemyKind));
    CHECK_FALSE(spawnableEnemy(kGargoyleEnemyKind));
    CHECK_FALSE(spawnableEnemy(kGeneralEnemyKind));
    CHECK_FALSE(spawnableEnemy(kEnemyKindCount));
}

TEST_CASE("spawn takes the map a visitor comes from after everything else",
          "[dev-console][commands]") {
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
    CHECK_FALSE(parseSpawn(Args{"gru", "1", "2", "e2", "x"}).has_value());
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
    CHECK_FALSE(parseSpawn(Args{"golem", "f1", "x"}).has_value());
    auto lamp = spawn({"L1XS_LAMPHO#12", "l1"});
    CHECK(lamp.kind == Kind::Thing);
    CHECK(lamp.name == "L1XS_LAMPHO#12");
    CHECK(lamp.map == "L1");
    CHECK_FALSE(spawn({"key"}).map.has_value());
}

TEST_CASE("spawn takes a category word first and a map before an object",
          "[dev-console][commands]") {
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
    const auto golem = parseSpawn(Args{"enemy", "golem"});
    REQUIRE_FALSE(golem.has_value());
    CHECK(golem.error().contains("No enemy kind named GOLEM"));
    CHECK_FALSE(parseSpawn(Args{"critter", "gru"}).has_value());
    const auto object = parseSpawn(Args{"object"});
    REQUIRE_FALSE(object.has_value());
    CHECK(object.error() == spawnUsage());
    CHECK_FALSE(parseSpawn(Args{"object", "a", "b", "c"}).has_value());
}

TEST_CASE("spawn generator special asks for a realm's mixed brood", "[dev-console][commands]") {
    const auto brood = parseSpawn(Args{"generator", "special", "e2"});
    REQUIRE(brood.has_value());
    CHECK(brood->kind == Kind::Generator);
    CHECK(brood->brood);
    CHECK(brood->enemyKind == -1);
    CHECK(brood->name == "SPECIAL");
    CHECK(brood->map == "E2");
    CHECK(brood->tier == 1);
    const auto tough = parseSpawn(Args{"generator", "SPECIAL", "3"});
    REQUIRE(tough.has_value());
    CHECK(tough->brood);
    CHECK(tough->tier == 3);
    CHECK_FALSE(tough->map.has_value());
    const auto grunts = parseSpawn(Args{"generator", "gru"});
    REQUIRE(grunts.has_value());
    CHECK_FALSE(grunts->brood);
    CHECK(spawnUsage().contains("<enemy>|special"));
}

TEST_CASE("a layout's generator record is found by name", "[dev-console][commands][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    const ItemInfo* grunts = generatorRecord(layout, "gru");
    REQUIRE(grunts != nullptr);
    CHECK(grunts->type == ItemInfo::kGenerator);
    CHECK(grunts->name == "GRU");
    CHECK(generatorRecord(layout, "NOTHING_OF_THE_KIND") == nullptr);
}

TEST_CASE("a layout's generator record is found by name without the disc",
          "[dev-console][commands]") {
    const WorldLayout layout = test::syntheticLayout();
    const ItemInfo* rats = generatorRecord(layout, "rat");
    REQUIRE(rats != nullptr);
    CHECK(rats->name == "RAT");
    CHECK(generatorRecord(layout, "GOLEM") == nullptr);
}

TEST_CASE("spawn's usage reads as it does today", "[dev-console][commands]") {
    CHECK(spawnUsage() == kSpawnUsage);
}

TEST_CASE("spawn refuses the words it cannot read as it does today", "[dev-console][commands]") {
    for (const Refusal& refusal :
         {Refusal{{}, kSpawnUsage}, Refusal{{"gru", "4"}, "A tier is 1 to 3"},
          Refusal{{"gru", "1", "0"}, "A count is 1 to 20"},
          Refusal{{"gru", "1", "2", "3"}, kSpawnUsage},
          Refusal{{"gru", "1", "2", "e2", "x"}, kSpawnUsage},
          Refusal{{"enemy", "golem"}, "No enemy kind named GOLEM; list enemies names them"},
          Refusal{{"critter", "gru"}, "A critter is a golem, a general or a gargoyle"},
          Refusal{{"gargoyle", "lion", "d1", "x"},
                  "A gargoyle is an eagl, a lion or a serp, then a map at most"},
          Refusal{{"golem", "f1", "x"}, kSpawnUsage},
          Refusal{{"generator", "golem"},
                  "No enemy kind named GOLEM for a generator; list enemies names them"},
          Refusal{{"generator", "rat", "1", "2"}, kSpawnUsage},
          Refusal{{"generator", "rat", "4"}, "A tier is 1 to 3"}, Refusal{{"object"}, kSpawnUsage},
          Refusal{{"object", "a", "b", "c"}, kSpawnUsage}}) {
        CAPTURE(refusal.words);
        const auto request = parseSpawn(refusal.words);
        CHECK_FALSE(request.has_value());
        if (!request.has_value()) {
            CHECK(request.error() == refusal.error);
        }
    }
}

} // namespace
