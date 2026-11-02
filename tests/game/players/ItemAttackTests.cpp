#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

#include "game/combat/DamageTypes.h"
#include "game/combat/MissileTarget.h"
#include "game/combat/PowerupFlags.h"
#include "game/players/ItemAttack.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("item attacks retain retail priority and elemental damage", "[game][items]") {
    PowerupEffects worn;
    CHECK_FALSE(ItemAttack::select(worn));
    worn.special = powerup::kBreath;
    CHECK(ItemAttack::select(worn)->tree == "FIREBREATHE");
    worn.weapon = damage::kHammer;
    CHECK(ItemAttack::select(worn)->deed == PlayerDeed::Hammer);
    worn.weapon |= damage::kSuperShot;
    CHECK(ItemAttack::select(worn)->deed == PlayerDeed::SuperShot);
    worn.special |= powerup::kRightGauntlet;
    CHECK(ItemAttack::select(worn)->deed == PlayerDeed::FireRight);
    worn.special |= powerup::kLeftGauntlet;
    CHECK(ItemAttack::select(worn)->deed == PlayerDeed::FireLeft);
    worn.special |= powerup::kSkorneMask;
    CHECK(ItemAttack::select(worn)->sound == "S_MASK");
    worn.special |= powerup::kSkorneHorns;
    CHECK(ItemAttack::select(worn)->sound == "S_HORNS");
    CHECK(ItemAttack::select(worn)->damage == 50);
    CHECK(ItemAttack::select(worn)->chargeKind == 0);
    constexpr std::array<u32, 3> kBreaths{powerup::kFireBreath, powerup::kAcidBreath,
                                          powerup::kLightningBreath};
    constexpr std::array<u32, 3> kElements{1, 4, 2};
    for (usize i = 0; i < kBreaths.size(); ++i) {
        worn = {};
        worn.special = kBreaths[i];
        const auto attack = ItemAttack::select(worn);
        REQUIRE(attack);
        CHECK(attack->flags == (32 | kElements[i]));
        CHECK(attack->damage == 40);
        CHECK(attack->chargeKind == powerup::kSpecial);
        CHECK(attack->chargeMask == powerup::kBreath);
        CHECK(attack->head);
    }
}

TEST_CASE("breath expands forwards while hammer damage waits for the impact delay",
          "[game][items]") {
    PowerupEffects worn;
    worn.special = powerup::kFireBreath;
    const auto breath = *ItemAttack::select(worn);
    const MissileTarget ahead{0, {0, 0, 6}, 1, 4};
    const MissileTarget behind{1, {0, 0, -6}, 1, 4};
    const MissileTarget beside{2, {6, 0, 0}, 1, 4};
    CHECK(breath.reaches(Mat4{1}, ahead, 0.1f, 1));
    CHECK_FALSE(breath.reaches(Mat4{1}, behind, 0.1f, 1));
    CHECK_FALSE(breath.reaches(Mat4{1}, beside, 0.1f, 1));
    CHECK_FALSE(breath.reaches(Mat4{1}, ahead, 0.8f, 1));
    CHECK(breath.damageAt(0.1f, 1) == Approx(34.2f));
    CHECK(breath.damageAt(0.5f, 1) < breath.damageAt(0.1f, 1));
    worn.weapon = damage::kHammer;
    const auto hammer = *ItemAttack::select(worn);
    CHECK_FALSE(hammer.reaches(Mat4{1}, ahead, 0.09f, 1));
    CHECK(hammer.reaches(Mat4{1}, behind, 0.1f, 1));
    CHECK(hammer.damageAt(0.1f, 1) == Approx(100.5f));
    CHECK(hammer.damageAt(0.8f, 1) == 0);
    CHECK_FALSE(hammer.reaches(Mat4{1}, ahead, 0, 0));
}

TEST_CASE("hammer expansion measures its active phase after the authored delay",
          "[game][items][alpha-hammer-phase][alpha-area-height]") {
    PowerupEffects worn;
    worn.weapon = damage::kHammer;
    const auto hammer = *ItemAttack::select(worn);
    const MissileTarget near{0, {0, 0, -11}, 0.25f, 2};
    const MissileTarget rim{1, {0, 0, 14}, 0.25f, 2};
    CHECK_FALSE(hammer.reaches(Mat4{1}, near, 0.099f, 1));
    CHECK(hammer.reaches(Mat4{1}, near, 0.1f, 1));
    CHECK_FALSE(hammer.reaches(Mat4{1}, rim, 0.1f, 1));
    CHECK(hammer.reaches(Mat4{1}, rim, 0.2f, 1));
    CHECK(hammer.damageAt(0.7f, 1) > 0);
    CHECK(hammer.damageAt(0.71f, 1) == 0);
    CHECK_FALSE(hammer.reaches(Mat4{1}, near, 0.71f, 1));
    // Item-family collision still tests height; only swarm areas ignore it.
    CHECK_FALSE(hammer.reaches(Mat4{1}, {2, {0, 40, 5}, 1, 4}, 0.5f, 1));
    CHECK_FALSE(hammer.reaches(Mat4{1}, {3, {0, -40, 5}, 1, 4}, 0.5f, 1));
    CHECK(hammer.damageAt(0.1f, 0.12f) == Approx(100.5f));
    CHECK(hammer.damageAt(0.1f, 0.1f) == 0);
}

TEST_CASE("an item attack digests its eleven fields in declaration order",
          "[game][items][native-digest]") {
    const ItemAttack attack{
        PlayerDeed::Hammer, "EXPRING", "S_THUNDERHAMMER", 100, 35, 0.1f, -1, 32, 7, 0x40, true};
    NativeStateDigest digest;
    digest.value(attack);

    NativeStateDigest expected;
    expected.word(static_cast<u32>(PlayerDeed::Hammer));
    expected.string("EXPRING");
    expected.string("S_THUNDERHAMMER");
    expected.real(100);
    expected.real(35);
    expected.real(0.1f);
    expected.real(-1);
    expected.word(32);
    expected.integer(7);
    expected.word(0x40);
    expected.flag(true);
    CHECK(digest.finish() == expected.finish());
}
} // namespace
