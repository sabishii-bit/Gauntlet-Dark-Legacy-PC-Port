#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/enemies/OpponentPool.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a combatant answers to the great ones' pool or the boss's, in a byte",
          "[game][level-opponents][opponent-pool]") {
    STATIC_CHECK(std::is_same_v<std::underlying_type_t<CombatantPool>, u8>);
    STATIC_CHECK(CombatantPool{} == CombatantPool::Critters);
    STATIC_CHECK(CombatantPool::Critters != CombatantPool::Boss);
}
} // namespace
