#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"

#include "game/combat/StrikeHit.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a hit reaches what stands within it, and within its arc when it has one",
          "[game][world][strikes]") {
    StrikeHit hit;
    hit.centre = Vec3{0.0f, 0.0f, 0.0f};
    hit.radius = 5.0f;
    hit.damage = 50.0f;
    hit.facing = Vec3{0.0f, 0.0f, 1.0f};
    REQUIRE(hit.reaches(Vec3{0.0f, 0.0f, 5.5f}, 1.0f, 3.0f));
    REQUIRE_FALSE(hit.reaches(Vec3{0.0f, 0.0f, 6.5f}, 1.0f, 3.0f));
    REQUIRE(hit.reaches(Vec3{0.0f, 0.0f, -4.0f}, 1.0f, 3.0f)); // all round
    REQUIRE_FALSE(hit.reaches(Vec3{0.0f, 9.0f, 1.0f}, 1.0f, 3.0f));
    REQUIRE(hit.reaches(Vec3{0.0f, -7.0f, 1.0f}, 1.0f, 3.0f)); // its top comes up into it
    hit.arc = 0.5f;                                            // sixty degrees either side of ahead
    REQUIRE(hit.reaches(Vec3{1.0f, 0.0f, 4.0f}, 1.0f, 3.0f));
    REQUIRE_FALSE(hit.reaches(Vec3{4.0f, 0.0f, 1.0f}, 1.0f, 3.0f));
    REQUIRE_FALSE(hit.reaches(Vec3{0.0f, 0.0f, -4.0f}, 1.0f, 3.0f));
    hit.damage = 0;
    CHECK_FALSE(hit.reaches(Vec3{0}, 1, 3));
    hit.damage = 10;
    hit.radius = 0;
    CHECK_FALSE(hit.reaches(Vec3{0}, 1, 3)); // a presentation-only burst has no damage volume
}
} // namespace
