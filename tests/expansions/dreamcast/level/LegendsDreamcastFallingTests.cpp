#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "expansions/dreamcast/level/LegendsDreamcastFalling.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::ItemInfoRecord rubbleInfo() {
    return {.type = 10,
            .subtype = 40,
            .collisionType = 1,
            .collisionFlags = 1,
            .radius = 2,
            .height = 4,
            .name = {},
            .activeType = 16,
            .choices = {}};
}

TEST_CASE("Dreamcast falling rubble waits for contact and collapses once for the whole party",
          "[legends][dreamcast-falling]") {
    LegendsDreamcastFalling state;
    state.clear(-20);
    const formats::ItemInstanceRecord item{.name = {}, .position = {10, 8, 20}};
    REQUIRE(state.add(rubbleInfo(), item, 12, 7));
    REQUIRE(state.pieces().size() == 1);
    CHECK(state.visible(7));
    CHECK(state.visible(100)); // unrelated fixture ownership is unchanged
    CHECK_FALSE(state.pieces()[0].contact.solid);
    state.update(2);
    CHECK(state.pieces()[0].body.position == item.position);
    CHECK_FALSE(state.pieces()[0].active);
    const auto touch = [&](LegendsPortalVisitor visitor) {
        return state.touch(std::array{visitor});
    };
    CHECK(touch({.position = {12.76f, 8, 20}}).empty());
    CHECK(touch({.position = {10, 20, 20}}).empty());
    CHECK(touch({.position = item.position, .alive = false}).empty());
    CHECK(touch({.player = 4, .position = item.position}).empty());
    CHECK(touch({.position = {std::numeric_limits<f32>::quiet_NaN(), 8, 20}}).empty());

    std::array<LegendsPortalVisitor, 4> players;
    for (usize seat = 0; seat < players.size(); ++seat) {
        players[seat] = {.player = static_cast<u8>(seat), .position = item.position};
    }
    const auto activated = state.touch(players);
    REQUIRE(activated.size() == 1);
    CHECK(activated.front() == item.position);
    CHECK(state.pieces()[0].active);
    CHECK(state.touch(players).empty());
    state.update(1.f / 60);
    CHECK(state.pieces()[0].body.position == item.position);
    state.update(1.f / 60);
    CHECK(std::abs(state.pieces()[0].body.position.y - (8 - 2.f / 30)) < .00001f);
    CHECK(state.pieces()[0].body.velocity.y == -2);
    CHECK(state.pieces()[0].body.rotation.x > 0);
    CHECK(state.pieces()[0].body.rotation.z < 0);
    state.update(1.f / 30);
    CHECK(std::abs(state.pieces()[0].body.position.y - 7.8f) < .00001f);
    state.update(10);
    CHECK_FALSE(state.visible(7));
    CHECK(state.touch(players).empty());
    state.clear(-20);
    CHECK(state.pieces().empty());
    REQUIRE(state.add(rubbleInfo(), item, 12, 7));
    CHECK(state.visible(7));
    CHECK_FALSE(state.pieces()[0].active);
    CHECK(state.pieces()[0].body.position == item.position);
}

TEST_CASE("Dreamcast contact-collapse adapter is scoped and independent of render cadence",
          "[legends][dreamcast-falling]") {
    LegendsDreamcastFalling fast;
    LegendsDreamcastFalling slow;
    const formats::ItemInstanceRecord item{.name = {}, .position = {0, 10, 0}};
    fast.clear(-50);
    slow.clear(-50);
    REQUIRE(fast.add(rubbleInfo(), item, 0, 3));
    REQUIRE(slow.add(rubbleInfo(), item, 0, 3));
    CHECK_FALSE(fast.add(rubbleInfo(), item, 0, 4));
    CHECK_FALSE(fast.add(rubbleInfo(), item, 1, 3));
    auto unsupported = rubbleInfo();
    unsupported.subtype = 51; // Skorne's scripted eruption owns this family.
    CHECK_FALSE(fast.add(unsupported, item, 1, 4));
    unsupported = rubbleInfo();
    unsupported.type = 8;
    CHECK_FALSE(fast.add(unsupported, item, 1, 4));
    unsupported = rubbleInfo();
    unsupported.radius = -1;
    CHECK_FALSE(fast.add(unsupported, item, 1, 4));
    auto overridden = item;
    overridden.params[0] = 51;
    CHECK_FALSE(fast.add(rubbleInfo(), overridden, 1, 4));
    // There is no Levitation exclusion in ItemTouch's falling-scenery branch.
    const std::array visitor{LegendsPortalVisitor{.position = item.position, .levitating = true}};
    REQUIRE(fast.touch(visitor).size() == 1);
    REQUIRE(slow.touch(visitor).size() == 1);
    for (s32 i = 0; i < 120; ++i) {
        fast.update(1.f / 120);
    }
    for (s32 i = 0; i < 30; ++i) {
        slow.update(1.f / 30);
    }
    CHECK(fast.pieces()[0].body == slow.pieces()[0].body);
    CHECK(std::abs(fast.pieces()[0].body.position.y - (10 - 31)) < .0001f);
}

TEST_CASE("Dreamcast sinking rock uses its native slow tumble without becoming a solid floor",
          "[legends][dreamcast-falling]") {
    LegendsDreamcastFalling sink;
    LegendsDreamcastFalling fall;
    sink.clear(-20);
    fall.clear(-20);
    auto info = rubbleInfo();
    info.subtype = 53;
    const formats::ItemInstanceRecord item{.name = {}, .position = {10, 8, 20}};
    REQUIRE(sink.add(info, item, 20, 7));
    REQUIRE(fall.add(rubbleInfo(), item, 20, 7));
    const std::array visitor{LegendsPortalVisitor{.position = item.position}};
    REQUIRE(sink.touch(visitor).size() == 1);
    REQUIRE(fall.touch(visitor).size() == 1);
    sink.update(1);
    fall.update(1);
    CHECK(sink.pieces()[0].body.position == fall.pieces()[0].body.position);
    CHECK(std::abs(sink.pieces()[0].body.rotation.x * 20 - fall.pieces()[0].body.rotation.x) <
          .0001f);
    CHECK(std::abs(sink.pieces()[0].body.rotation.z * 20 - fall.pieces()[0].body.rotation.z) <
          .0001f);
    CHECK_FALSE(sink.pieces()[0].contact.solid);
    CHECK(sink.touch(visitor).empty());
}
} // namespace
