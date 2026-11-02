#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/PowerupFlags.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the powerup kinds are the original's item subtypes", "[game][powerup-flags]") {
    STATIC_REQUIRE(std::is_same_v<decltype(powerup::kWeapon), const s32>);
    STATIC_REQUIRE(std::is_same_v<decltype(powerup::kSpecial), const s32>);
    CHECK(powerup::kWeapon == 5);
    CHECK(powerup::kArmor == 6);
    CHECK(powerup::kSpeed == 7);
    CHECK(powerup::kMagic == 8);
    CHECK(powerup::kSpecial == 9);
}

TEST_CASE("the special word keeps the original's SPECIAL_TYPE bits", "[game][powerup-flags]") {
    STATIC_REQUIRE(std::is_same_v<decltype(powerup::kLevitation), const u32>);
    STATIC_REQUIRE(std::is_same_v<decltype(powerup::kBreath), const u32>);
    STATIC_REQUIRE(std::is_same_v<decltype(powerup::kHealthVamp), const u32>);
    CHECK(powerup::kLevitation == 0x1U);
    CHECK(powerup::kXRay == 0x2U);
    CHECK(powerup::kInvisible == 0x4U);
    CHECK(powerup::kStopTime == 0x8U);
    CHECK(powerup::kFireBreath == 0x10U);
    CHECK(powerup::kAcidBreath == 0x20U);
    CHECK(powerup::kLightningBreath == 0x40U);
    CHECK(powerup::kBreath == 0x70U);
    CHECK(powerup::kPhoenix == 0x80U);
    CHECK(powerup::kGrowth == 0x100U);
    CHECK(powerup::kEnemyShrink == 0x200U);
    CHECK(powerup::kPojo == 0x400U);
    CHECK(powerup::kSkorneHorns == 0x1000U);
    CHECK(powerup::kSkorneMask == 0x2000U);
    CHECK(powerup::kRightGauntlet == 0x4000U);
    CHECK(powerup::kLeftGauntlet == 0x8000U);
    CHECK(powerup::kSpeedBoost == 0x10000U);
    CHECK(powerup::kHealth == 0x20000U);
    CHECK(powerup::kDummy == 0x40000U);
    CHECK(powerup::kTurbo == 0x80000U);
    CHECK(powerup::kMikey == 0x100000U);
    CHECK(powerup::kHandOfDeath == 0x200000U);
    CHECK(powerup::kHealthVamp == 0x400000U);
    // The specials that bring a companion.
    STATIC_REQUIRE((powerup::kLevitation | powerup::kBreath | powerup::kPhoenix | powerup::kPojo |
                    powerup::kMikey | powerup::kHandOfDeath | powerup::kHealthVamp) == 0x7004F1U);
}

} // namespace
