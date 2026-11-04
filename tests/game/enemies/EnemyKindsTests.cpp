#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/enemies/EnemyKinds.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the enemy kinds are known by name, in tiers of health", "[game][enemies]") {
    // The grunt: a tall walker of thirty health, hitting for fifteen, chasing round corners.
    const EnemyKind& grunt = enemyKind(kGruntKind);
    REQUIRE(grunt.name == "GRU");
    REQUIRE(grunt.prefix == "GRU");
    REQUIRE(grunt.height == 6.0f);
    REQUIRE(grunt.radius == 1.5f);
    REQUIRE(grunt.pace == Approx(0.1f));
    REQUIRE(grunt.damage == 15.0f);
    REQUIRE(grunt.health == 30.0f);
    REQUIRE(grunt.armor == 0.0f);
    REQUIRE(grunt.generatorArmor == 3.0f);
    REQUIRE(grunt.experienceHit == 2);
    REQUIRE(grunt.experienceKill == 4);
    REQUIRE(grunt.algorithm == 7);
    REQUIRE(grunt.turnRate == Approx(0.0491f).margin(0.0001f));
    // A tier is a third of the full health, up to three.
    REQUIRE(grunt.healthAtTier(1) == Approx(9.99f));
    REQUIRE(grunt.healthAtTier(2) == Approx(19.98f));
    REQUIRE(grunt.healthAtTier(3) == Approx(29.97f));
    REQUIRE(grunt.healthAtTier(9) == Approx(29.97f));
    // The rat: small, quick to turn on whoever comes near.
    const EnemyKind& rat = enemyKind(kRatKind);
    REQUIRE(rat.name == "RAT");
    REQUIRE(rat.height == 3.0f);
    REQUIRE(rat.algorithm == 2);
    REQUIRE(rat.experienceKill == 2);
    // Death is the one armoured kind, and the gargoyle the one worth three hundred.
    REQUIRE(enemyKind(kDeathKind).armor == 1.0f);
    REQUIRE(enemyKind(32).experienceKill == 300);
    REQUIRE(enemyKind(33).name == "GENERAL");
    REQUIRE(enemyKind(33).prefix == "GEN");
    // Names are matched in any case, by name or prefix; the sentinel row and strangers are not.
    REQUIRE(enemyKindOf("GRU") == kGruntKind);
    REQUIRE(enemyKindOf("gru") == kGruntKind);
    REQUIRE(enemyKindOf("General") == 33);
    REQUIRE(enemyKindOf("GEN") == 33);
    REQUIRE_FALSE(enemyKindOf("NONE").has_value());
    REQUIRE_FALSE(enemyKindOf("BOSSGEN").has_value());
    REQUIRE_FALSE(enemyKindOf("").has_value());
    REQUIRE(enemyKind(-1).name == enemyKind(0).name);
    REQUIRE(enemyKind(99).name == enemyKind(kEnemyKindCount - 1).name);
}

TEST_CASE("the scorpion alone walks when nothing else is asked, and the acid blob alone is rooted",
          "[game][enemies]") {
    // do_enemies' default action (enemy.c 5262) and fn_8004DC2C's E_ACID cases.
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        CHECK(enemyKind(kind).idle == (kind == 0 ? EnemyAction::Walk : EnemyAction::Ready));
        CHECK(enemyKind(kind).rooted == (kind == 21));
    }
    CHECK(enemyKindOf("SCO") == 0);
    CHECK(enemyKindOf("ACI") == 21);
}

TEST_CASE("a level breeds its roster's kinds for the classes its generators name",
          "[game][enemies]") {
    // The fields: zombies for the medium, maggots for the small, and no large at all.
    const std::vector<LevelEnemy> fields{{13, kMediumClass, {}},
                                         {13, kMediumOtherClass, {}},
                                         {12, kSmallClass, {}},
                                         {29, 5, {}},
                                         {33, 5, {}},
                                         {32, 5, {}}};
    REQUIRE(levelKindOf(fields, kGruntKind, 1) == 13);
    REQUIRE(levelKindOf(fields, kGruntKind, 3) == 13);
    REQUIRE(levelKindOf(fields, kGruntKind, 4) == 13);     // the second row, the same kind here
    REQUIRE(levelKindOf(fields, kGruntKind + 1, 2) == 13); // a knight stands for the medium too
    REQUIRE(levelKindOf(fields, kRatKind, 1) == 12);
    REQUIRE(levelKindOf(fields, 33, 1) == 33); // the great ones stand for themselves
    // A roster without a medium falls back on its large; without either, the name itself.
    const std::vector<LevelEnemy> sparse{{20, kLargeClass, {}}, {0, kSmallClass, {}}};
    REQUIRE(levelKindOf(sparse, kGruntKind, 1) == 20);
    REQUIRE(levelKindOf(sparse, kRatKind, 1) == 0);
    REQUIRE(levelKindOf({}, kGruntKind, 1) == kGruntKind);
    REQUIRE(levelKindOf({}, kRatKind, 1) == kRatKind);
    // The strong variants take the medium's second row when the realm has one.
    const std::vector<LevelEnemy> castle{{4, kMediumClass, {}},
                                         {19, kMediumOtherClass, {}},
                                         {3, kSmallClass, {}},
                                         {5, kLargeClass, {}}};
    REQUIRE(levelKindOf(castle, kGruntKind, 5) == 19);
    REQUIRE(levelKindOf(castle, kGruntKind, 2) == 4);
}

TEST_CASE("only the original's small kinds are small, whatever the code",
          "[game][enemies][enemy-traits]") {
    // The kinds of height three: scorpion, rat, snake, spider, maggot, wolf, dog, acid, hand.
    constexpr std::array<s32, 9> kSmall{0, 3, 6, 9, 12, 15, 18, 21, 22};
    std::vector<s32> kinds{std::numeric_limits<s32>::min(), -1000, 1000,
                           std::numeric_limits<s32>::max()};
    for (s32 kind = -2; kind <= 46; ++kind) {
        kinds.push_back(kind);
    }
    for (const s32 kind : kinds) {
        CAPTURE(kind);
        CHECK(smallKind(kind) == std::ranges::contains(kSmall, kind));
    }
}

TEST_CASE("the original's enemy types name their own kind rows", "[game][enemies][enemy-traits]") {
    struct Row {
        s32 type;
        std::string_view name;
    };
    constexpr std::array<Row, 33> kRows{{
        {enemy_type::kScorpion, "SCO"},    {enemy_type::kTroll, "TRO"},
        {enemy_type::kDemon, "DEM"},       {enemy_type::kRat, "RAT"},
        {enemy_type::kGrunt, "GRU"},       {enemy_type::kKnight, "KNI"},
        {enemy_type::kSnake, "SNA"},       {enemy_type::kSorcerer, "SOR"},
        {enemy_type::kMummy, "MUM"},       {enemy_type::kSpider, "SPI"},
        {enemy_type::kLizardman, "LIZ"},   {enemy_type::kTreefolk, "TRE"},
        {enemy_type::kMaggot, "MAG"},      {enemy_type::kZombie, "ZOM"},
        {enemy_type::kPlague, "PLA"},      {enemy_type::kWolf, "WOL"},
        {enemy_type::kIce, "ICE"},         {enemy_type::kWorm, "WRM"},
        {enemy_type::kDog, "DOG"},         {enemy_type::kSkeleton, "SKE"},
        {enemy_type::kGhost, "GHO"},       {enemy_type::kAcid, "ACI"},
        {enemy_type::kHand, "HAN"},        {enemy_type::kImp, "IMP"},
        {enemy_type::kWarlock, "WAR"},     {enemy_type::kSky, "SKY"},
        {enemy_type::kWhirlwind, "WIND"},  {enemy_type::kGarm2, "GRM"},
        {enemy_type::kGolem, "GOLEM"},     {enemy_type::kDeath, "DEATH"},
        {enemy_type::kIt, "IT"},           {enemy_type::kGargoyle, "GAR"},
        {enemy_type::kGeneral, "GENERAL"},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.type);
        CHECK(enemyKind(row.type).name == row.name);
        CHECK(enemyKindOf(row.name) == row.type);
    }
}

TEST_CASE("only the original's tiered kinds take a sound set a tier whatever the code",
          "[game][enemies][enemy-traits]") {
    // The kinds whose hit and bite stems end in the digit of their tier's set.
    constexpr std::array<s32, 18> kTiered{1,  2,  4,  5,  7,  8,  10, 11, 13,
                                          14, 16, 17, 19, 20, 23, 24, 25, 26};
    std::vector<s32> kinds{std::numeric_limits<s32>::min(), -1000, 1000,
                           std::numeric_limits<s32>::max()};
    for (s32 kind = -2; kind <= 46; ++kind) {
        kinds.push_back(kind);
    }
    for (const s32 kind : kinds) {
        CAPTURE(kind);
        CHECK(tieredSounds(kind) == std::ranges::contains(kTiered, kind));
    }
}

TEST_CASE("every name a level gives breeds its roster's kind of that class at every strength",
          "[game][enemies][enemy-traits]") {
    constexpr s32 kStrongFrom = 4; ///< the strengths that take the medium's second row
    // What a name breeds under strength four and from it; a name a roster's rows do not list
    // breeds itself.
    struct Row {
        s32 named;
        s32 below;
        s32 fromFour;
    };
    struct Roster {
        std::string_view name;
        std::vector<LevelEnemy> enemies;
        std::vector<Row> rows;
    };
    const std::vector<Roster> rosters{
        {"every class twice and a temple row",
         {{12, kSmallClass, {}},
          {15, kSmallClass, {}},
          {13, kMediumClass, {}},
          {16, kMediumClass, {}},
          {19, kMediumOtherClass, {}},
          {25, kMediumOtherClass, {}},
          {20, kLargeClass, {}},
          {23, kLargeClass, {}},
          {14, 12, {}}},
         {{3, 12, 12}, {4, 13, 19}, {5, 13, 19}}},
        {"a large and a small but no medium",
         {{20, kLargeClass, {}}, {0, kSmallClass, {}}},
         {{3, 0, 0}, {4, 20, 20}, {5, 20, 20}}},
        {"only the medium's second row", {{19, kMediumOtherClass, {}}}, {{4, 4, 19}, {5, 5, 19}}},
        {"no roster", {}, {}},
        {"every class unset",
         {{-1, kSmallClass, {}},
          {-1, kMediumClass, {}},
          {-1, kMediumOtherClass, {}},
          {-1, kLargeClass, {}}},
         {}},
    };
    std::vector<s32> names{std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max()};
    for (s32 named = -1; named <= 45; ++named) {
        names.push_back(named);
    }
    for (const Roster& roster : rosters) {
        for (const s32 named : names) {
            const auto row = std::ranges::find(roster.rows, named, &Row::named);
            for (s32 strength = -1; strength <= 7; ++strength) {
                CAPTURE(roster.name, named, strength);
                s32 expected = named;
                if (row != roster.rows.end()) {
                    expected = strength >= kStrongFrom ? row->fromFour : row->below;
                }
                CHECK(levelKindOf(roster.enemies, named, strength) == expected);
            }
        }
    }
}

} // namespace
