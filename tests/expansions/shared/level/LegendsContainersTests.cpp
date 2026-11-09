#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsN64ContainerTables.h"
#include "expansions/shared/level/LegendsContainers.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using NativeTables = test::N64ContainerTables;
} // namespace

TEST_CASE("Native barrels require one sufficient hit and release once at animation completion",
          "[legends][containers]") {
    LegendsContainer barrel{NativeTables{}.decode()};
    REQUIRE(barrel.targetable());
    REQUIRE(barrel.obstacle().solid);
    for (usize i = 0; i < 20; ++i) {
        CHECK_FALSE(barrel.strike(5.99f));
    }
    CHECK_FALSE(barrel.strike(std::numeric_limits<f32>::infinity()));
    CHECK_FALSE(barrel.strike(std::numeric_limits<f32>::quiet_NaN()));
    REQUIRE(barrel.strike(6));
    CHECK_FALSE(barrel.targetable());
    CHECK_FALSE(barrel.obstacle().solid);
    CHECK_FALSE(barrel.strike(9999));
    CHECK(barrel.state() == LegendsContainer::State::Opening);
    REQUIRE(barrel.finishOpening());
    CHECK_FALSE(barrel.finishOpening());
    CHECK(barrel.state() == LegendsContainer::State::Open);
    barrel.collected();
    CHECK(barrel.state() == LegendsContainer::State::Empty);
    CHECK_FALSE(barrel.obstacle().solid);
}

TEST_CASE("Native chests spend exactly one key on contact and ignore attacks",
          "[legends][containers]") {
    NativeTables tables;
    tables.chest();
    LegendsContainer chest{tables.decode()};
    s32 keys = 2;
    CHECK_FALSE(chest.strike(9999));
    CHECK_FALSE(chest.touch({50, 2, 9}, .75f, 4, keys));
    CHECK_FALSE(chest.touch({5, 8, 9}, .75f, 4, keys));
    CHECK(keys == 2);
    SECTION("a taller player can reach a chest above their feet") {
        REQUIRE(chest.touch({5, -1, 9}, .75f, 4, keys));
        CHECK(keys == 1);
        return;
    }
    keys = 0;
    CHECK_FALSE(chest.touch({5, 2, 9}, .75f, 4, keys));
    keys = 2;
    REQUIRE(chest.touch({5, 2, 9}, .75f, 4, keys));
    CHECK(keys == 1);
    CHECK(chest.obstacle().solid);
    CHECK(chest.obstacle().blocksSegment({5, 3, 5}, {5, 3, 13}, .5f));
    CHECK_FALSE(chest.touch({5, 2, 9}, .75f, 4, keys));
    CHECK(keys == 1);
    REQUIRE(chest.finishOpening());
    CHECK_FALSE(chest.obstacle().solid);
}
