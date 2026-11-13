#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"
#include "game/characters/Vitality.h"

namespace {

using namespace gdl;
using namespace gdl::game;

CharacterSave atLevel(s32 level, s32 health) {
    CharacterSave save;
    save.progress().experience = levelExperience(level);
    save.progress().health = health;
    return save;
}

TEST_CASE("a character's most health is five hundred and a hundred a level, never past 9999",
          "[vitality][health-limit]") {
    // player_max_health (0x8007857C) over the status box's limit (0x80347788).
    CHECK(kHealthLimit == 9999);
    CHECK(mostHealth(1) == 500);
    CHECK(mostHealth(10) == 1400);
    CHECK(mostHealth(95) == 9900);
    CHECK(mostHealth(96) == kHealthLimit);
    CHECK(mostHealth(kMaxLevel) == kHealthLimit);
}

TEST_CASE("a heal tops health up to the level's most and gives nothing once it is there",
          "[vitality][health-limit]") {
    SECTION("below the most, as much as fits") {
        auto save = atLevel(1, 450);
        CHECK_FALSE(healthFull(save));
        CHECK(heal(save, 100) == 50);
        CHECK(save.health() == 500);
        CHECK(healthFull(save));
        CHECK(heal(save, 100) == 0);
        CHECK(save.health() == 500);
    }
    SECTION("at the top level, no further than 9999, however much is offered") {
        auto save = atLevel(kMaxLevel, 1);
        CHECK(heal(save, 1'000'000) == kHealthLimit - 1);
        CHECK(save.health() == kHealthLimit);
    }
    SECTION("over the most, from what raised it past, nothing") {
        auto save = atLevel(1, 600);
        CHECK(healthFull(save));
        CHECK(heal(save, 100) == 0);
        CHECK(save.health() == 600);
    }
    SECTION("a class never played starts from the health it shows") {
        auto save = atLevel(5, 0);
        REQUIRE(save.health() == kStartingHealth);
        CHECK(heal(save, 100) == 100);
        CHECK(save.health() == 600);
    }
    SECTION("nothing, or less, gives nothing") {
        auto save = atLevel(5, 400);
        CHECK(heal(save, 0) == 0);
        CHECK(heal(save, -50) == 0);
        CHECK(save.health() == 400);
    }
}

TEST_CASE("health given past the level's most starts from the health the character shows",
          "[vitality][health-limit]") {
    // AddExp's level gained (0x80076400) and the halo's point (0x8004E8FC) add outright.
    auto played = atLevel(1, 500);
    addHealth(played, 100);
    CHECK(played.health() == 600);
    auto fresh = atLevel(1, 0);
    REQUIRE(fresh.health() == kStartingHealth);
    addHealth(fresh, 100);
    CHECK(fresh.health() == 600);
}

TEST_CASE("stored health over the limit is put back to it, and a class never played kept",
          "[vitality][health-limit]") {
    ClassProgress progress;
    progress.health = 10400;
    limitHealth(progress);
    CHECK(progress.health == kHealthLimit);
    progress.health = 1200;
    limitHealth(progress);
    CHECK(progress.health == 1200);
    progress.health = 0;
    limitHealth(progress);
    CHECK(progress.health == 0);
    progress.health = 1200;
    limitHealth(progress, 1000);
    CHECK(progress.health == 1000);
}

} // namespace
