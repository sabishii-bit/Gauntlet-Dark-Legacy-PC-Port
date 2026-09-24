#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/Damage.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("elemental amulets use retail affinity and encounter multipliers",
          "[game][items][damage]") {
    for (u32 element = 1; element <= 4; ++element) {
        CAPTURE(element);
        const u32 own = 1U << (element - 1);
        const u32 opposite = 1U << ((element - 1) ^ 1U);
        CHECK(Damage::modify(20, element, 0, 4, false).amount == Approx(24));
        CHECK(Damage::modify(20, element, own, 4, false).amount == Approx(8));
        CHECK(Damage::modify(20, element, own << 8, 4, false).amount == 0);
        CHECK(Damage::modify(20, element, opposite, 4, false).amount == Approx(32));
        CHECK(Damage::modify(20, element, 0, 4, true).amount == Approx(20));
        CHECK(Damage::modify(20, element, own, 4, true).amount == Approx(12));
        CHECK(Damage::modify(20, element, opposite, 4, true).amount == Approx(24));
    }
}
TEST_CASE("protective items distinguish immunity healing and knockback protection",
          "[game][items][damage]") {
    CHECK(Damage::modify(100, 1, Damage::kInvulnerable, 0, false).amount == 0);
    CHECK(Damage::modify(100, 1, Damage::kGoldInvulnerable, 0, false).amount == Approx(-10));
    CHECK(Damage::modify(1, 0, Damage::kGoldInvulnerable, 0, false).amount == 0);
    CHECK(Damage::modify(20, Damage::kGas, 0x2000, 0, false).amount == 0);
    CHECK(Damage::modify(20, 0, 0x2000, 0, false).amount == 20);
    CHECK(Damage::modify(20, Damage::kMagic, 0x1000, 0, false).amount == 0);
    CHECK(Damage::modify(20, Damage::kMagic, 0x10, 100, false).amount == 10);
    CHECK(Damage::modify(20, Damage::kMagic, 0x10, 100, true).amount == 15);
    const auto braced = Damage::modify(20, 0x10170 | 1, 0x40000, 0, false);
    CHECK(braced.flags == 1);
    CHECK(braced.amount == 30);
    CHECK(Damage::modify(20, 0, 0, 100, false).amount == 0);
    CHECK(Damage::modify(20, Damage::kGas, 0, 100, false).amount == 20);
}
} // namespace
