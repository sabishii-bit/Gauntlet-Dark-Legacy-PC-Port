#include <limits>
#include <stdexcept>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsTowerMotion.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("Legends Tower Sumner cylinder blocks, slides, and permits escape",
          "[legends][tower][motion][sumner]") {
    const formats::LegendsN64TowerVolume body{
        formats::LegendsN64TowerVolume::Shape::Cylinder, {6, 3, -8}, {1.5f, 3, 1.5f}, 0, 7};
    const auto resolve = [&](Vec3 from, Vec3 to, f32 height = 5) {
        return resolveLegendsTowerVolume(body, from, to, .75f, height);
    };
    REQUIRE(resolve({10, 0, -8}, {6, 0, -8}) == Vec3(8.25f, 0, -8));
    REQUIRE(resolve({10, 0, -8}, {2, 0, -8}) == Vec3(8.25f, 0, -8));
    REQUIRE(resolve({10, 7, -8}, {2, 7, -8}) == Vec3(2, 7, -8));
    REQUIRE(resolve({10, -6, -8}, {2, -6, -8}) == Vec3(2, -6, -8));
    REQUIRE(resolve({10, -6, -8}, {2, -6, -8}, 7) == Vec3(8.25f, -6, -8));
    REQUIRE(resolve({7, 0, -8}, {9, 0, -8}) == Vec3(9, 0, -8));
    REQUIRE(resolve({7, 0, -8}, {3, 0, -8}) == Vec3(7, 0, -8));
    const Vec3 from{10, 0, -8};
    const Vec3 to{7, 0, -6};
    const Vec3 slide = resolve(from, to);
    REQUIRE(slide != from);
    REQUIRE(slide != to);
    REQUIRE(glm::length(slide - from) <= glm::length(to - from));
    REQUIRE(glm::length(Vec2{slide.x - 6, slide.z + 8}) >= 2.25f - .00001f);
}

TEST_CASE("Legends Tower podium uses its rotated box with rounded player contacts",
          "[legends][tower][motion][sumner]") {
    const formats::LegendsN64TowerVolume box{
        formats::LegendsN64TowerVolume::Shape::Box, {6, 2.5f, -8}, {.5f, 2.5f, .625f}, -.731f, 14};
    const auto world = [&](Vec3 local) {
        const f32 c = std::cos(box.yaw);
        const f32 s = std::sin(box.yaw);
        return box.centre + Vec3{c * local.x + s * local.z, local.y, -s * local.x + c * local.z};
    };
    const auto resolve = [&](Vec3 from, Vec3 to) {
        return resolveLegendsTowerVolume(box, world(from), world(to), .5f, 2);
    };
    REQUIRE(resolve({2, -2.5f, 0}, {-2, -2.5f, 0}) == world({2, -2.5f, 0}));
    REQUIRE(resolve({2, 3, 0}, {-2, 3, 0}) == world({-2, 3, 0}));
    REQUIRE(resolve({2, -5, 0}, {-2, -5, 0}) == world({-2, -5, 0}));
    // Clear of the actual rounded corner, but inside a radius-expanded AABB.
    REQUIRE(resolve({.9f, -2.5f, 1.1f}, {.85f, -2.5f, 1.05f}) == world({.85f, -2.5f, 1.05f}));
    REQUIRE(resolve({1.5f, -2.5f, 1.5f}, {.7f, -2.5f, .8f}) == world({1.5f, -2.5f, 1.5f}));
    // A pre-existing overlap may leave, but cannot cross through the podium.
    REQUIRE(resolve({.8f, -2.5f, 0}, {2, -2.5f, 0}) == world({2, -2.5f, 0}));
    REQUIRE(resolve({.8f, -2.5f, 0}, {-2, -2.5f, 0}) == world({.8f, -2.5f, 0}));
    REQUIRE(resolve({0, 2.4f, 0}, {0, 4, 0}) == world({0, 4, 0}));
    REQUIRE(resolve({0, 2.4f, 0}, {0, 0, 0}) == world({0, 2.4f, 0}));
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    REQUIRE(resolveLegendsTowerVolume(box, {10, 0, -8}, {nan, 0, 0}, .5f, 2) == Vec3(10, 0, -8));
    REQUIRE_THROWS_AS(resolveLegendsTowerVolume(box, {nan, 0, 0}, {}, .5f, 2),
                      std::invalid_argument);
}

TEST_CASE("Legends Tower locked doors block swept player bodies", "[legends][tower][motion]") {
    formats::LegendsN64TowerDoor door;
    door.triggerRadius = 9;
    REQUIRE(resolveLegendsTowerDoor(door, {12, 0, 0}, {8, 0, 0}, 1, 3) == Vec3(10, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {12, 0, 0}, {-12, 0, 0}, 1, 3) == Vec3(10, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {12, 0, 0}, {10, 0, 0}, 1, 3) == Vec3(10, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {12, 0, 0}, {14, 0, 0}, 1, 3) == Vec3(14, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {12, 0, 0}, {11, 0, 0}, 1, 3) == Vec3(11, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {12, 10, 0}, {-12, 10, 0}, 1, 3) == Vec3(-12, 10, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {12, -13, 0}, {-12, -13, 0}, 1, 3) == Vec3(-12, -13, 0));
    // The player's actual height matters; a tall character overlaps the volume.
    REQUIRE(resolveLegendsTowerDoor(door, {12, -13, 0}, {-12, -13, 0}, 1, 5) == Vec3(10, -13, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {0, 20, 0}, {0, 0, 0}, 1, 3) == Vec3(0, 20, 0));
}

TEST_CASE("Legends Tower door sliding is bounded and does not trap existing overlaps",
          "[legends][tower][motion]") {
    formats::LegendsN64TowerDoor door;
    door.triggerRadius = 9;
    const Vec3 from{12, 0, 0};
    const Vec3 to{8, 0, 4};
    const auto result = resolveLegendsTowerDoor(door, from, to, 1, 3);
    REQUIRE(result != from);
    REQUIRE(result != to);
    REQUIRE(glm::length(result - from) <= glm::length(to - from));
    REQUIRE(glm::dot(result - from, to - from) > 0);
    REQUIRE(result.x * result.x + result.z * result.z >= 100 - .0001f);
    REQUIRE(result.y == 0);
    REQUIRE(result.z > 0);
    REQUIRE(resolveLegendsTowerDoor(door, {8, 0, 0}, {9, 0, 0}, 1, 3) == Vec3(9, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {8, 0, 0}, {7, 0, 0}, 1, 3) == Vec3(8, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {8, 0, 0}, {-12, 0, 0}, 1, 3) == Vec3(8, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {0, 0, 0}, {1, 0, 0}, 1, 3) == Vec3(1, 0, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {8, 0, 0}, {8, 10, 0}, 1, 3) == Vec3(8, 10, 0));
    REQUIRE(resolveLegendsTowerDoor(door, {8, 0, 0}, {8, 0, 0}, 1, 3) == Vec3(8, 0, 0));
}

TEST_CASE("Legends Tower door contact translates with its native anchor and rejects bad input",
          "[legends][tower][motion]") {
    formats::LegendsN64TowerDoor door;
    door.triggerPosition = {42, 2, -24};
    door.triggerRadius = 9;
    REQUIRE(resolveLegendsTowerDoor(door, {54, 2, -24}, {50, 2, -24}, 1, 3) == Vec3(52, 2, -24));
    const Vec3 from{54, 2, -24};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    REQUIRE(resolveLegendsTowerDoor(door, from, {nan, 0, 0}, 1, 3) == from);
    REQUIRE(resolveLegendsTowerDoor(door, from, {50, 2, -24}, nan, 3) == from);
    REQUIRE(resolveLegendsTowerDoor(door, from, {50, 2, -24}, 1, -1) == from);
    REQUIRE_THROWS_AS(resolveLegendsTowerDoor(door, {nan, 0, 0}, from, 1, 3),
                      std::invalid_argument);
}

TEST_CASE("Legends Tower idle waits for animation completion", "[legends][tower][motion]") {
    LegendsTowerIdle idle;
    REQUIRE(idle.initialSequence() == "LKDN");
    REQUIRE_FALSE(idle.update(false, 0));
    REQUIRE(idle.update(true, 32767)->sumner == "READ");
    REQUIRE_FALSE(idle.update(false, 0));
    REQUIRE_THROWS_AS(idle.update(true, 32768), std::invalid_argument);
}

TEST_CASE("Legends Tower idle retains native random selection boundaries",
          "[legends][tower][motion]") {
    for (const u16 value : {u16{0}, u16{983}, u16{984}, u16{6553}, u16{6554}, u16{32767}}) {
        DYNAMIC_SECTION(value) {
            LegendsTowerIdle idle;
            const auto cue = idle.update(true, value);
            REQUIRE(cue.has_value());
            if (value <= 983) {
                REQUIRE(cue->sumner == "CNJ1");
                REQUIRE(cue->podium.empty());
            } else if (value <= 6553) {
                REQUIRE(cue->sumner == "PAGE");
                REQUIRE(cue->podium == "FLIP");
            } else {
                REQUIRE(cue->sumner == "READ");
                REQUIRE(cue->podium.empty());
            }
        }
    }
}

TEST_CASE("Legends Tower conjuring plays one through four middle cycles",
          "[legends][tower][motion]") {
    for (u16 loops = 1; loops <= 4; ++loops) {
        DYNAMIC_SECTION(loops) {
            LegendsTowerIdle idle;
            REQUIRE(idle.update(true, 0)->sumner == "CNJ1");
            REQUIRE_FALSE(idle.update(false, 0));
            REQUIRE(idle.update(true, static_cast<u16>((loops - 1) << 13))->sumner == "CNJ2");
            for (u16 i = 1; i < loops; ++i) {
                REQUIRE(idle.update(true, 0)->sumner == "CNJ2");
            }
            REQUIRE(idle.update(true, 0)->sumner == "CNJ3");
            REQUIRE_FALSE(idle.update(false, 0));
            REQUIRE(idle.update(true, 32767)->sumner == "READ");
        }
    }
}

TEST_CASE("Legends Tower realm doors wait for an explicit progression event",
          "[legends][tower][motion]") {
    LegendsTowerDoorMotion door;
    REQUIRE(door.held());
    door.update(true);
    REQUIRE(door.state() == LegendsTowerDoorMotion::State::Closed);
    REQUIRE(door.unlock());
    REQUIRE_FALSE(door.held());
    REQUIRE_FALSE(door.unlock());
    door.update(false);
    REQUIRE(door.state() == LegendsTowerDoorMotion::State::Opening);
    door.update(true);
    REQUIRE(door.state() == LegendsTowerDoorMotion::State::Open);
    REQUIRE_FALSE(door.unlock());
    LegendsTowerDoorMotion installed(true);
    REQUIRE(installed.state() == LegendsTowerDoorMotion::State::Open);
    REQUIRE_FALSE(installed.held());
    REQUIRE_FALSE(installed.unlock());
}
