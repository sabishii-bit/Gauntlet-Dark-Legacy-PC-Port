#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "game/players/PlayerTransport.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a transporter fades for half a second and relocates once at its midpoint",
          "[transporters]") {
    const s32 step = GENERATE(1, 2, 3, 5, 30, 60);
    PlayerTransport transport;
    const Vec3 destination{10, 2, 3};
    CHECK(transport.alpha() == 1.0f);
    REQUIRE(transport.begin(destination));
    CHECK_FALSE(transport.begin(Vec3{20}));
    s32 elapsed = 0;
    s32 relocations = 0;
    while (transport.active()) {
        elapsed += step;
        if (const auto arrival = transport.update(step)) {
            CHECK(*arrival == destination);
            CHECK(elapsed > 15);
            ++relocations;
        }
        CHECK(transport.alpha() >= 0.0f);
        CHECK(transport.alpha() <= 1.0f);
    }
    CHECK(elapsed >= 30);
    CHECK(elapsed < 30 + step);
    CHECK(relocations == 1);
    CHECK(transport.alpha() == 1.0f);
    CHECK_FALSE(transport.update(60));
    transport.observeContact(true);
    CHECK_FALSE(transport.begin(destination));
    transport.observeContact(false);
    CHECK(transport.begin(destination));
}

TEST_CASE("transport transparency holds the invisible boundary and cancels safely",
          "[transporters]") {
    PlayerTransport transport;
    REQUIRE(transport.begin(Vec3{1}));
    CHECK_FALSE(transport.update(-1));
    CHECK(transport.alpha() == 1.0f);
    CHECK_FALSE(transport.update(15));
    CHECK(transport.alpha() == 0.0f);
    CHECK(transport.update(1).has_value());
    CHECK(transport.alpha() == Catch::Approx(1.0f / 29));
    transport.observeContact(false); // stepping off during the fade does not rearm
    transport.cancel();
    CHECK_FALSE(transport.armed());
    CHECK(transport.alpha() == 1.0f);
    transport.clear();
    CHECK(transport.armed());
}
} // namespace
