#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/EffectFade.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a spreading effect holds what it harms a second at least, three past its age through "
          "what it pierces",
          "[game][effect-fade]") {
    // ProcessEffects: an effect flagged 0x20 has fade 1 under an age of a second, its age past
    // it, and DMG_SUPER three more than its age (sfx.c 2706-2714).
    STATIC_REQUIRE(effect_fade::spreading(0.5f, false) == 1.0f);
    STATIC_REQUIRE(effect_fade::spreading(1.5f, false) == 1.5f);
    STATIC_REQUIRE(effect_fade::spreading(0.5f, true) == 3.5f);
    STATIC_REQUIRE(effect_fade::spreading(1.5f, true) == 4.5f);
}

TEST_CASE("a flying effect holds what it harms a fifth of a second at least, three and a quarter "
          "through what it pierces",
          "[game][effect-fade]") {
    // A flying effect's age is a quarter second, or its hit effect's frames over thirty; under a
    // fifth it holds a fifth, and DMG_SUPER keeps the quarter and holds three more (sfx.c
    // 2690-2714).
    STATIC_REQUIRE(effect_fade::flying(effect_fade::kShot, false) == 0.25f);
    STATIC_REQUIRE(effect_fade::flying(0.1f, false) == 0.2f);
    STATIC_REQUIRE(effect_fade::flying(22.0f / 30.0f, false) == 22.0f / 30.0f);
    STATIC_REQUIRE(effect_fade::flying(22.0f / 30.0f, true) == 3.25f);
}
} // namespace
