#include <array>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/dreamcast/level/LegendsDreamcastObelisks.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast obelisk solid bounds match its native pickup volume",
          "[legends][dreamcast-obelisks]") {
    formats::ItemInfoRecord info;
    info.type = 1;
    info.subtype = 12;
    info.name = "OBELISK";
    info.collisionType = 3;
    info.radius = 3;
    info.height = 8;
    info.xSize = 1.5f;
    info.zSize = 2;
    info.activeType = 0x16;
    formats::ItemInstanceRecord placement;
    placement.position = {12, 5, 30};
    placement.rotation.y = .3f;
    for (u8 identity = 0; identity < 9; ++identity) {
        placement.params[0] = identity;
        const auto definition = legendsDreamcastObelisk(info, placement, 23);
        REQUIRE(definition);
        const auto& body = definition->body;
        CHECK(definition->pickup.nativeIndex == 23);
        REQUIRE(definition->pickup.award);
        CHECK(definition->pickup.award->kind == LegendsRelic::Obelisk);
        CHECK(definition->pickup.award->index == identity);
        CHECK(definition->pickup.staysVisible);
        CHECK(body.centre == placement.position);
        CHECK(body.yaw == placement.rotation.y);
        CHECK(body.halfAcross == Catch::Approx(1.65f));
        CHECK(body.halfAlong == Catch::Approx(2.2f));
        CHECK(body.height == 8);
        CHECK(body.cylinderRadius == 0);
        CHECK(body.solid);
        CHECK(body.pushOut(placement.position, .5f) != placement.position);
        CHECK(body.blocksSegment(placement.position + Vec3{-5, 1, 0},
                                 placement.position + Vec3{5, 1, 0}, .5f));
        CHECK_FALSE(body.blocksSegment(placement.position + Vec3{-5, 10, 0},
                                       placement.position + Vec3{5, 10, 0}, .5f));
    }
    std::string diagnostic;
    SECTION("Out of range identity") {
        placement.params[0] = 9;
    }
    SECTION("Unsupported tilted body") {
        placement.rotation.x = .2f;
    }
    SECTION("Invalid body extent") {
        info.xSize = 0;
    }
    SECTION("Nonfinite origin") {
        placement.position.y = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("Wrong pickup type") {
        info.subtype = 10;
    }
    CHECK_FALSE(legendsDreamcastObelisk(info, placement, 23, &diagnostic));
}
} // namespace
