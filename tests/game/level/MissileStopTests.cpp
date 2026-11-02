#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/level/ItemFigure.h"
#include "game/level/MissileStop.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a missile stop made of a box is that box and no safe rock", "[missile-stop]") {
    STATIC_CHECK(std::is_same_v<decltype(MissileStop::rock), s32>);
    STATIC_CHECK(std::is_same_v<decltype(MissileStop::rockHealth), s32>);
    STATIC_CHECK(std::is_same_v<decltype(MissileStop::rockArmor), s32>);
    Obstacle box;
    box.centre = Vec3{4, 0, -2};
    box.halfAcross = 1.5f;
    box.halfAlong = 0.5f;
    box.height = 3;
    const MissileStop stop = MissileStop::of(box);
    CHECK(stop.box == box);
    CHECK(stop.rock == -1);
    CHECK(stop.rockHealth == 0);
    CHECK(stop.rockArmor == 0);
}
} // namespace
