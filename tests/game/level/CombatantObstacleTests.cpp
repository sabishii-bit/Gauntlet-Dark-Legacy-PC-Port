#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/level/CombatantObstacle.h"
#include "game/level/ItemFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a combatant obstacle blocks unless told otherwise and names no breakable",
          "[combatant-obstacle]") {
    const CombatantObstacle obstacle;
    CHECK(obstacle.box == Obstacle{});
    CHECK(obstacle.kind == CombatantObstacle::Kind::Blocks);
    CHECK(obstacle.id == -1);
    CHECK(obstacle.health == 0);
    CHECK(obstacle.armor == 0);
    CHECK_FALSE(obstacle.explodes);
}

TEST_CASE("a combatant obstacle's fields keep their types and its kinds their byte codes",
          "[combatant-obstacle]") {
    STATIC_CHECK(std::is_same_v<decltype(CombatantObstacle::box), Obstacle>);
    STATIC_CHECK(std::is_same_v<decltype(CombatantObstacle::id), s32>);
    STATIC_CHECK(std::is_same_v<decltype(CombatantObstacle::health), s32>);
    STATIC_CHECK(std::is_same_v<decltype(CombatantObstacle::armor), s32>);
    STATIC_CHECK(std::is_same_v<decltype(CombatantObstacle::explodes), bool>);
    STATIC_CHECK(std::is_same_v<std::underlying_type_t<CombatantObstacle::Kind>, u8>);
    STATIC_CHECK(static_cast<u8>(CombatantObstacle::Kind::Blocks) == 0);
    STATIC_CHECK(static_cast<u8>(CombatantObstacle::Kind::Chest) == 1);
    STATIC_CHECK(static_cast<u8>(CombatantObstacle::Kind::Breakable) == 2);
    STATIC_CHECK(static_cast<u8>(CombatantObstacle::Kind::LowGenerator) == 3);
}
} // namespace
