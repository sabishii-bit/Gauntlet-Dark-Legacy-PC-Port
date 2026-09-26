#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/enemies/DeathRules.h"

using namespace gdl::game;
TEST_CASE("Death variants have distinct drains and legend potion rewards", "[death]") {
    // GUNE5D 80046140 / 800761B0 / 8004E6F8. DEATH1's skin is red,
    // DEATH2's is black; DEATH_ARC uses RED_DEATH_ARC and DEATH_EXP does not.
    CHECK(DeathRules::form(1) == DeathForm::Red);
    CHECK(DeathRules::form(2) == DeathForm::Black);
    CHECK(DeathRules::effect(DeathForm::Red) == "DEATH_ARC");
    CHECK(DeathRules::effect(DeathForm::Black) == "DEATH_EXP");
    CHECK(DeathRules::experience(1, true) == 10);
    CHECK(DeathRules::experience(30, true) == 27);
    CHECK(DeathRules::experience(60, true) == 45);
    CHECK(DeathRules::experience(98, true) == 46);
    CHECK(DeathRules::experience(99, true) == 0);
    CHECK(DeathRules::experience(99, false) == 46);
    CHECK(DeathRules::magicHealing(75, 100) == 0);
    CHECK(DeathRules::magicHealing(76, 100) == Catch::Approx(23.2f));
    CHECK(DeathRules::magicHealing(99, 50) == Catch::Approx(48.4f));
}
