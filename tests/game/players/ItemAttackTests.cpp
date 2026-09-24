#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

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
    worn.weapon = powerup::kThunderHammer;
    CHECK(ItemAttack::select(worn)->deed == PlayerDeed::Hammer);
    worn.weapon |= powerup::kSuperShot;
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
    worn.weapon = powerup::kThunderHammer;
    const auto hammer = *ItemAttack::select(worn);
    CHECK_FALSE(hammer.reaches(Mat4{1}, ahead, 0.09f, 1));
    CHECK(hammer.reaches(Mat4{1}, behind, 0.1f, 1));
    CHECK(hammer.damageAt(0.1f, 1) == Approx(85.5f));
    CHECK(hammer.damageAt(0.8f, 1) == 0);
    CHECK_FALSE(hammer.reaches(Mat4{1}, ahead, 0, 0));
}
} // namespace
