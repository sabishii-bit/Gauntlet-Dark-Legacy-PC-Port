#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCollision.h"

#include "game/party/MeleeGeometry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

MeleeGeometry::Target cylinder(s32 id, const Vec3& base) {
    MeleeGeometry::Target result;
    result.id = id;
    result.instance = static_cast<u64>(id) + 1;
    result.base = base;
    result.radius = 1;
    result.height = 6;
    result.acquisition = {base + Vec3{0, 3, 0}, 1, 1, 10};
    result.contact = true;
    return result;
}

TEST_CASE("raw melee contacts preserve reversed slot ties and lower native grid cells",
          "[player-attacks][melee-geometry]") {
    std::array targets{cylinder(0, {0, 0, 1}), cylinder(1, {0, 0, -1})};
    const Vec3 origin{0, 3, 0};
    auto selected = MeleeGeometry::contact(targets, origin, origin, 2, 3);
    REQUIRE(selected);
    CHECK(selected->index == 1);
    CHECK(selected->endpointDistance == 0);
    targets[0].contactCell = 1;
    targets[1].contactCell = 2;
    selected = MeleeGeometry::contact(targets, origin, origin, 2, 3);
    REQUIRE(selected);
    CHECK(selected->index == 0);
    targets[0].contact = false;
    selected = MeleeGeometry::contact(targets, origin, origin, 2, 3);
    REQUIRE(selected);
    CHECK(selected->index == 1);
}

TEST_CASE("raw melee contact retains endpoint overlap escape and swept distance rules",
          "[player-attacks][melee-geometry]") {
    const std::array targets{cylinder(7, {0, 0, 0})};
    constexpr f32 kWidth = 1;
    const auto inward = MeleeGeometry::contact(targets, {-1, 3, 0}, {-0.5f, 3, 0}, kWidth, 3);
    REQUIRE(inward);
    CHECK(inward->endpointDistance == 0);
    const auto through = MeleeGeometry::contact(targets, {-1, 3, 0}, {0.5f, 3, 0}, kWidth, 3);
    REQUIRE(through);
    CHECK(through->endpointDistance == Catch::Approx(0.5f));
    CHECK_FALSE(MeleeGeometry::contact(targets, {-1, 3, 0}, {-1.5f, 3, 0}, kWidth, 3));
    CHECK_FALSE(MeleeGeometry::contact(targets, {-10, 3, 0}, {10, 3, 0}, kWidth, 3));
    CHECK_FALSE(MeleeGeometry::contact(targets, {0, 6, 0}, {0, 6, 0}, kWidth, 3));
    CHECK_FALSE(MeleeGeometry::contact(targets, {2, 3, 0}, {2, 3, 0}, kWidth, 3));
}

TEST_CASE("a wall-rejected melee contact never promotes an overlapping runner-up",
          "[player-attacks][melee-geometry]") {
    const std::array targets{cylinder(0, {1, 0, 0}), cylinder(1, {0, 0, 2})};
    WorldCollision collision;
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 1}, Vec3{10, 0, 1}, Vec3{0, 20, 1}}}});
    const Vec3 origin{0, 3, 0};
    REQUIRE(MeleeGeometry::contact(std::span{targets}.first(1), origin, origin, 2, 3, &collision));
    REQUIRE(MeleeGeometry::contact(targets, origin, origin, 2, 3));
    CHECK_FALSE(MeleeGeometry::contact(targets, origin, origin, 2, 3, &collision));
}

TEST_CASE("moving melee geometry recomputes distance vertical range and authored acquisition",
          "[player-attacks][melee-geometry]") {
    std::array targets{cylinder(0, {0, 0, 8}), cylinder(1, {1, 0, 8})};
    targets[0].contact = targets[1].contact = false;
    targets[0].low = true;
    targets[1].acquisition.distanceScale = 0.5f;
    const auto query = [&](const Vec3& feet) {
        const Vec3 middle = feet + Vec3{0, 3, 0};
        return MeleeGeometry::snapshot(targets, feet, 6, 2, 30, 0.707f, middle, middle, middle);
    };
    CHECK(query({0, 0, 0}).targets.empty());
    const auto moved = query({0, 0, 4});
    REQUIRE(moved.targets.size() == 2);
    CHECK(moved.targets[0].distance == 3);
    CHECK(moved.targets[0].offset == Vec3{0, 0, 4});
    const auto selected = MeleeSelection::select(moved, {0, 0, 1}, 5);
    REQUIRE(selected);
    CHECK(selected->index == 1);
    CHECK(query({0, 6, 4}).targets.empty());
    targets[1].acquisition.enabled = false;
    REQUIRE(query({0, 0, 4}).targets.size() == 1);
    targets[0].acquisition.maxHeight = 0;
    CHECK(query({0, 1, 4}).targets.empty());
}

TEST_CASE("moving melee keeps the attempted sweep distinct from final contact refinement",
          "[player-attacks][melee-geometry]") {
    const std::array targets{cylinder(0, {0, 0, 0})};
    const Vec3 feet{4, 0, 0};
    const Vec3 follow = feet + Vec3{0, 3, 0};
    const auto swept =
        MeleeGeometry::snapshot(targets, feet, 6, 2, 30, 0.707f, follow, {-1, 3, 0}, {0.5f, 3, 0});
    REQUIRE(swept.contact);
    CHECK(swept.contact->target.offset == Vec3{-4, 0, 0});
    CHECK(swept.contact->target.distance == 3);
    const auto stationary =
        MeleeGeometry::snapshot(targets, feet, 6, 2, 30, 0.707f, follow, follow, follow);
    CHECK_FALSE(stationary.contact);
}

TEST_CASE("moving melee aim retains contact priority and Death's overlapping facing gate",
          "[player-attacks][melee-geometry]") {
    std::array targets{cylinder(0, {0, 0, 3}), cylinder(1, {0, 0, -0.5f})};
    targets[0].contact = false;
    const Vec3 middle{0, 3, 0};
    const auto query = [&] {
        return MeleeGeometry::snapshot(targets, {0, 0, 0}, 6, 2, 30, 0.707f, middle, middle,
                                       middle);
    };
    CHECK(MeleeGeometry::aim(targets, middle, {0, 0, 1}, 30, nullptr, 0.707f, query()) ==
          targets[1].acquisition.point);
    targets[1].requireFacing = true;
    CHECK(MeleeGeometry::aim(targets, middle, {0, 0, 1}, 30, nullptr, 0.707f, query()) ==
          targets[0].acquisition.point);
    targets[0].acquisition.enabled = false;
    CHECK_FALSE(MeleeGeometry::aim(targets, middle, {0, 0, 1}, 30, nullptr, 0.707f, query()));
}

TEST_CASE("raw melee candidates preserve native mesh and unweighted cylinder distances",
          "[player-attacks][melee-geometry]") {
    const std::array<CollisionTriangle, 1> surface{
        {{{0, 0, -1}, {Vec3{-10, 0, 8}, Vec3{10, 0, 8}, Vec3{0, 20, 8}}}}};
    MissileTarget mesh{5, {0, 0, 8}, 5, 12, surface};
    const auto plain = MeleeGeometry::candidate({0, 0, 0}, 6, mesh, true);
    CHECK(plain.distance == Catch::Approx(8));
    CHECK(plain.score == plain.distance);
    CHECK(plain.offset == Vec3{0, 0, 8});
    CHECK(plain.low);
    mesh.acquisition = MissileTarget::Acquisition{{0, 6, 8}, 2, 1.2f, 12};
    const auto weighted = MeleeGeometry::candidate({0, 0, 0}, 6, mesh, false);
    CHECK(weighted.distance == plain.distance);
    CHECK(weighted.score == Catch::Approx(glm::length(Vec3{0, 3, 8}) * 1.2f - 2));
    CHECK_FALSE(weighted.low);
}
} // namespace
