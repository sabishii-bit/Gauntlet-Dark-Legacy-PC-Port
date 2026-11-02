#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/ArmorFlags.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the armour word keeps the original's ARMOR_TYPE bits", "[game][armor-flags]") {
    STATIC_REQUIRE(std::is_same_v<decltype(armor::kResistFire), const u32>);
    STATIC_REQUIRE(std::is_same_v<decltype(armor::kReflects), const u32>);
    CHECK(armor::kResistFire == 0x1U);
    CHECK(armor::kResistElectric == 0x2U);
    CHECK(armor::kResistLight == 0x4U);
    CHECK(armor::kResistAcid == 0x8U);
    CHECK(armor::kResistMagic == 0x10U);
    CHECK(armor::kImmuneFire == 0x100U);
    CHECK(armor::kImmuneElectric == 0x200U);
    CHECK(armor::kImmuneLight == 0x400U);
    CHECK(armor::kImmuneAcid == 0x800U);
    CHECK(armor::kImmuneMagic == 0x1000U);
    CHECK(armor::kImmuneGas == 0x2000U);
    CHECK(armor::kInvulnerable == 0x10000U);
    CHECK(armor::kReflect == 0x20000U);
    CHECK(armor::kNoKnockback == 0x40000U);
    CHECK(armor::kAntiDeath == 0x80000U);
    CHECK(armor::kGoldInvulnerable == 0x100000U);
    CHECK(armor::kFireArmor == 0x200000U);
    CHECK(armor::kElectricArmor == 0x400000U);
    CHECK(armor::kProtect == 0x800000U);
    CHECK(armor::kReflect2 == 0x1000000U);
    CHECK(armor::kArmShields == 0x620000U);
    CHECK(armor::kStandsFirm == 0x150000U);
    CHECK(armor::kReflects == 0x01020000U);
}

TEST_CASE("an element's resistance and immunity are its bit and that bit shifted",
          "[game][armor-flags]") {
    STATIC_REQUIRE(armor::resist(1) == armor::kResistFire);
    STATIC_REQUIRE(armor::resist(4) == armor::kResistAcid);
    STATIC_REQUIRE(armor::immune(2) == armor::kImmuneElectric);
    STATIC_REQUIRE(armor::immune(3) == armor::kImmuneLight);
}

} // namespace
