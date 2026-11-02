#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/EffectFlags.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the effect word keeps the SFXX row's bits", "[game][effect-flags]") {
    STATIC_REQUIRE(std::is_same_v<decltype(effect_flags::kBodyParent), const u32>);
    STATIC_REQUIRE(std::is_same_v<decltype(effect_flags::kFollows), const u32>);
    CHECK(effect_flags::kBodyParent == 0x1U);
    CHECK(effect_flags::kShakes == 0x2U);
    CHECK(effect_flags::kOnFloor == 0x10U);
    CHECK(effect_flags::kArenaCue == 0x20U);
    CHECK(effect_flags::kDetached == 0x40U);
    CHECK(effect_flags::kAtInitialRoot == 0x80U);
    CHECK(effect_flags::kSkin == 0x100U);
    CHECK(effect_flags::kHidesBody == 0x400U);
    CHECK(effect_flags::kRootParent == 0x800U);
    CHECK(effect_flags::kFollows == 0x801U);
    CHECK(effect_flags::kOtherParent == 0x2000U);
    CHECK(effect_flags::kTrail == 0x4000U);
    CHECK(effect_flags::kShrinks == 0x10000U);
    CHECK(effect_flags::kLeavesGenerator == 0x20000U);
    CHECK(effect_flags::kUntilNextMove == 0x40000U);
    CHECK(effect_flags::kSummonsEnemies == 0x400000U);
    CHECK(effect_flags::kNoLoop == 0x800000U);
    CHECK(effect_flags::kParticleMask == 0x0F000000U);
    CHECK(effect_flags::kShortParticle == 0x02000000U);
    // The placements a turbo strike's effects cannot take.
    STATIC_REQUIRE((effect_flags::kOnFloor | effect_flags::kDetached |
                    effect_flags::kAtInitialRoot | effect_flags::kRootParent |
                    effect_flags::kOtherParent | effect_flags::kUntilNextMove) == 0x428D0U);
}

} // namespace
