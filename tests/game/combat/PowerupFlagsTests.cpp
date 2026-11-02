#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/PowerupFlags.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether every one of `Constants` is a constant `T`. */
template <class T, class... Constants>
constexpr bool kAllOfType = std::conjunction_v<std::is_same<Constants, const T>...>;

TEST_CASE("the powerup kinds are the original's item subtypes", "[game][powerup-flags]") {
    STATIC_REQUIRE(kAllOfType<s32, decltype(powerup::kWeapon), decltype(powerup::kArmor),
                              decltype(powerup::kSpeed), decltype(powerup::kMagic),
                              decltype(powerup::kSpecial)>);
    CHECK(powerup::kWeapon == 5);
    CHECK(powerup::kArmor == 6);
    CHECK(powerup::kSpeed == 7);
    CHECK(powerup::kMagic == 8);
    CHECK(powerup::kSpecial == 9);
}

TEST_CASE("the special word keeps the original's SPECIAL_TYPE bits", "[game][powerup-flags]") {
    STATIC_REQUIRE(
        kAllOfType<u32, decltype(powerup::kLevitation), decltype(powerup::kXRay),
                   decltype(powerup::kInvisible), decltype(powerup::kStopTime),
                   decltype(powerup::kFireBreath), decltype(powerup::kAcidBreath),
                   decltype(powerup::kLightningBreath), decltype(powerup::kBreath),
                   decltype(powerup::kPhoenix), decltype(powerup::kGrowth),
                   decltype(powerup::kEnemyShrink), decltype(powerup::kPojo),
                   decltype(powerup::kSkorneHorns), decltype(powerup::kSkorneMask),
                   decltype(powerup::kRightGauntlet), decltype(powerup::kLeftGauntlet),
                   decltype(powerup::kSpeedBoost), decltype(powerup::kHealth),
                   decltype(powerup::kDummy), decltype(powerup::kTurbo), decltype(powerup::kMikey),
                   decltype(powerup::kHandOfDeath), decltype(powerup::kHealthVamp)>);
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
}

TEST_CASE("a wrong type among the constants fails the type check", "[game][powerup-flags]") {
    STATIC_REQUIRE_FALSE(
        kAllOfType<u32, decltype(powerup::kLevitation), decltype(powerup::kArmor)>);
    STATIC_REQUIRE_FALSE(kAllOfType<s32, decltype(powerup::kWeapon), decltype(powerup::kTurbo)>);
}

} // namespace
