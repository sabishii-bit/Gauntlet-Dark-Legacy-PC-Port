#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

} // namespace
