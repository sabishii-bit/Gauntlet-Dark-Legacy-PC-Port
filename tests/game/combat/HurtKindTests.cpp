#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/DamageTypes.h"
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

TEST_CASE("a trap's hurt comes from a direction exactly when it knocks back or down",
          "[game][hurt-kind]") {
    constexpr u32 kDirected = damage::kKnockBack | damage::kKnockDown;
    STATIC_CHECK(std::is_same_v<std::underlying_type_t<HurtSource>, u8>);
    STATIC_CHECK(hurtSourceOf(0) == HurtSource::Undirected);
    STATIC_CHECK(hurtSourceOf(damage::kKnockBack) == HurtSource::Directed);
    STATIC_CHECK(hurtSourceOf(damage::kKnockDown) == HurtSource::Directed);
    STATIC_CHECK(hurtSourceOf(kDirected) == HurtSource::Directed);
    // The other knocks, a stun, a spike and the elements leave a hurt from nowhere in particular.
    STATIC_CHECK(hurtSourceOf(damage::kKnockOver) == HurtSource::Undirected);
    STATIC_CHECK(hurtSourceOf(damage::kBlownAway) == HurtSource::Undirected);
    STATIC_CHECK(hurtSourceOf(damage::kWhirlwind) == HurtSource::Undirected);
    STATIC_CHECK(hurtSourceOf(damage::kStun | damage::kSpike) == HurtSource::Undirected);
    STATIC_CHECK(hurtSourceOf(damage::kElementMask) == HurtSource::Undirected);
    STATIC_CHECK(hurtSourceOf(~kDirected) == HurtSource::Undirected);
    // Either knock still counts beside anything else.
    STATIC_CHECK(hurtSourceOf(damage::kFire | damage::kKnockDown) == HurtSource::Directed);
    STATIC_CHECK(hurtSourceOf(damage::kSpike | damage::kKnockBack) == HurtSource::Directed);
    STATIC_CHECK(hurtSourceOf(~damage::kKnockDown) == HurtSource::Directed);
    STATIC_CHECK(hurtSourceOf(~damage::kKnockBack) == HurtSource::Directed);
}
} // namespace
