#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/HurtKind.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the hurt kinds keep the byte values the hurt's sound scope counts its cue from",
          "[game][hurt-kind]") {
    STATIC_CHECK(std::is_same_v<std::underlying_type_t<HurtKind>, u8>);
    STATIC_CHECK(static_cast<u8>(HurtKind::Blow) == 0);
    STATIC_CHECK(static_cast<u8>(HurtKind::Burn) == 1);
    STATIC_CHECK(static_cast<u8>(HurtKind::Pierce) == 2);
    STATIC_CHECK(static_cast<u8>(HurtKind::Gas) == 3);
    STATIC_CHECK(static_cast<u8>(HurtKind::QuietBlow) == 4);
    STATIC_CHECK(static_cast<u8>(HurtKind::DeathDrain) == 5);
}
} // namespace
