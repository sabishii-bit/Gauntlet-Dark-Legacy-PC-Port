#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/Breakables.h"
#include "game/world/TargetAssist.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("item aim anchors keep authored offsets while capping only acquisition radii",
          "[game][target-assist][alpha-aim-acquisition]") {
    const Mat4 placement = itemPlacement({10, 20, 30}, {0, 0, 1.57079637f});
    const auto rotated = TargetAssist::itemAcquisition(placement, {1, 2, 3}, 8, 7, 1.2f);
    CHECK(glm::distance(rotated.point, Vec3{7, 21, 33}) < 0.001f);
    CHECK(rotated.radius == 5);
    CHECK(rotated.maxHeight == 14);
    CHECK(rotated.distanceScale == Approx(1.2f));
    const auto vertical = TargetAssist::itemAcquisition(placement, {0, 2, 0}, 2, 7, 1);
    CHECK(vertical.point == Vec3{10, 23, 30});
    CHECK(vertical.radius == 2);
    const auto threshold = TargetAssist::itemAcquisition(placement, {0.01f, 2, 0}, 2, 7, 0.9f);
    CHECK(threshold.point == Vec3{10.01f, 23, 30});
}

TEST_CASE("barrel snapshots retain ordinary and explosive acquisition weights without resizing",
          "[game][target-assist][alpha-aim-acquisition]") {
    const auto directory = test::scratchDirectory("barrel-aim-metadata");
    writeTextFile(directory / "world.json",
                  R"({"objects":[{"name":"ROOT","position":[0,0,0]}],"itemInfos":[
      {"type":10,"subtype":43,"name":"BARREL","radius":8,"height":4,"collisionOffset":[0,2,0]},
      {"type":10,"subtype":44,"name":"BARREL","radius":8,"height":4,"collisionOffset":[0,2,0]},
      {"type":10,"subtype":45,"name":"BARREL","radius":8,"height":4,"collisionOffset":[0,2,0]}],
      "itemInstances":[{"info":0,"position":[0,10,20]},
                       {"info":1,"position":[30,10,20]},
                       {"info":2,"position":[60,10,20]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    ItemArchive noArtwork;
    test::FakeRenderDevice device;
    Breakables barrels;
    REQUIRE(barrels.bind(device, layout, noArtwork, nullptr));
    REQUIRE(barrels.size() == 3);
    for (usize index = 0; index < barrels.size(); ++index) {
        const auto target = barrels.target(index, static_cast<s32>(index));
        REQUIRE(target.acquisition);
        CHECK(target.radius == 8);
        CHECK(target.height == 4);
        CHECK(target.acquisition->point == target.base + Vec3{0, 3, 0});
        CHECK(target.acquisition->radius == 5);
        CHECK(target.acquisition->maxHeight == 8);
        CHECK(target.acquisition->distanceScale == Approx(index == 0 ? 1.2f : 0.9f));
    }
}

TEST_CASE("Easy targeting widens native acquisition but preserves explicit normal defaults",
          "[game][target-assist][alpha-aim-acquisition]") {
    const Vec3 origin{0, 3, 0};
    const Vec3 forward{0, 0, 1};
    MissileTarget target{0, {6, 0, 8}, 1, 6};
    target.acquisition = MissileTarget::Acquisition{{6, 3, 8}, 1, 1, 10};
    const std::span targets{&target, 1};
    CHECK(TargetAssist::select(origin, forward, targets, 30));
    target.acquisition->point = {8, 3, 6};
    CHECK_FALSE(TargetAssist::select(origin, forward, targets, 30));
    CHECK_FALSE(TargetAssist::select(origin, forward, targets, 30, nullptr, 0.707f));
    // .5 + 9*.5/30=.65: this bearing still misses even on Easy.
    CHECK_FALSE(TargetAssist::select(origin, forward, targets, 30, nullptr, 0.5f));
    target.acquisition->point = {6, 3, 6};
    CHECK_FALSE(TargetAssist::select(origin, forward, targets, 30));
    CHECK(TargetAssist::select(origin, forward, targets, 30, nullptr, 0.5f));
    target.acquisition.reset();
    target.base = {6, 0, 6};
    target.node = 0;
    CHECK_FALSE(TargetAssist::select(origin, forward, targets, 30));
    CHECK(TargetAssist::select(origin, forward, targets, 30, nullptr, 0.5f));
}

TEST_CASE("native acquisition narrows with distance and rejects another enemy floor",
          "[game][target-assist][alpha-aim-acquisition]") {
    constexpr f32 kDot = 0.79f;
    const Vec3 forward{0, 0, 1};
    MissileTarget target{0, {0, 0, 10}, 1, 6};
    target.acquisition =
        MissileTarget::Acquisition{{10 * std::sqrt(1 - kDot * kDot), 3, 10 * kDot}, 1, 1, 10};
    // closest_enemy 800445D8: .707 + (distance-radius)*(1-.707)/range.
    // At range30 this ten-unit target misses; the boss range keeps it.
    CHECK_FALSE(TargetAssist::select({0, 3, 0}, forward, std::span{&target, 1}, 30));
    CHECK(TargetAssist::select({0, 3, 0}, forward, std::span{&target, 1}, 200) ==
          target.acquisition->point);
    target.acquisition->point = {0, 13, 10};
    CHECK(TargetAssist::select({0, 3, 0}, forward, std::span{&target, 1}, 30));
    target.acquisition->point.y += 0.01f;
    CHECK_FALSE(TargetAssist::select({0, 3, 0}, forward, std::span{&target, 1}, 30));
    target.acquisition->point = {0, -7.01f, 10};
    CHECK_FALSE(TargetAssist::select({0, 3, 0}, forward, std::span{&target, 1}, 30));
    // Acquisition never changes the collider, nor the old unannotated query.
    CHECK(target.pointNear({0, 3, 0}) == Vec3{0, 3, 10});
    CHECK(target.touches({0, 3, 10}, 0));
    target.acquisition.reset();
    CHECK(TargetAssist::select({0, 3, 0}, forward, std::span{&target, 1}, 30) == Vec3{0, 3, 10});
}

TEST_CASE("item acquisition weights distance before its radius and cone",
          "[game][target-assist][alpha-aim-acquisition]") {
    const Vec3 origin{0, 3, 0};
    const Vec3 forward{0, 0, 1};
    std::array targets{MissileTarget{0, {0, 0, 10}, 1, 6}, MissileTarget{1, {1, 0, 9}, 1, 6}};
    targets[0].acquisition = MissileTarget::Acquisition{{0, 3, 10}, 1, 1, 10};
    targets[1].acquisition = MissileTarget::Acquisition{{1, 3, 9}, 1, 1.2f, 12};
    // Ordinary items are weighted1.2 before radius subtraction (fn_8005B274).
    CHECK(TargetAssist::select(origin, forward, targets, 30) == targets[0].acquisition->point);
    targets[1].acquisition->distanceScale = 0.9f; // explosive / poison barrels
    CHECK(TargetAssist::select(origin, forward, targets, 30) == targets[1].acquisition->point);
    targets[1].acquisition->distanceScale = 1; // generator
    CHECK(TargetAssist::select(origin, forward, targets, 30) == targets[1].acquisition->point);
    // The item-height limit is independent of the enemy's ten-unit limit.
    targets[1].acquisition->point = {1, 15, 9};
    CHECK(TargetAssist::select(origin, forward, std::span{&targets[1], 1}, 30));
    targets[1].acquisition->point.y += 0.01f;
    CHECK_FALSE(TargetAssist::select(origin, forward, std::span{&targets[1], 1}, 30));
    constexpr f32 kDot = 0.79f;
    targets[1].acquisition->point = {10 * std::sqrt(1 - kDot * kDot), 3, 10 * kDot};
    targets[1].acquisition->distanceScale = 1.2f;
    CHECK_FALSE(TargetAssist::select(origin, forward, std::span{&targets[1], 1}, 30));
    targets[1].acquisition->distanceScale = 0.9f;
    CHECK(TargetAssist::select(origin, forward, std::span{&targets[1], 1}, 30));
}

TEST_CASE("melee acquisition shares item priorities without weighting its physical contact",
          "[game][target-assist][alpha-aim-acquisition]") {
    std::array targets{MissileTarget{0, {0, 0, 3.2f}, 1, 6}, MissileTarget{1, {0, 0, 3}, 1, 6}};
    targets[0].acquisition = MissileTarget::Acquisition{{0, 3, 3.2f}, 1, 1, 10};
    targets[1].acquisition = MissileTarget::Acquisition{{0, 3, 3}, 1, 1.2f, 12};
    const auto selected = TargetAssist::ahead({}, 6, {0, 0, 1}, targets, 3, 30);
    REQUIRE(selected);
    CHECK(selected->id == 0);
    const auto contact = TargetAssist::around({}, 6, targets, 3);
    REQUIRE(contact);
    CHECK(contact->id == 1); // the nearer physical body, independent of aim weights
    targets[1].acquisition->distanceScale = 0.9f;
    const auto explosive = TargetAssist::ahead({}, 6, {0, 0, 1}, targets, 3, 30);
    REQUIRE(explosive);
    CHECK(explosive->id == 1);
    CHECK_FALSE(TargetAssist::ahead({}, 6, {0, 0, 1}, targets, 1, 30));
    CHECK_FALSE(TargetAssist::ahead({}, 6, {0, 0, -1}, targets, 3, 30));
}

TEST_CASE("native acquisition points do not replace secret-wall surfaces or visibility",
          "[game][target-assist][alpha-aim-acquisition]") {
    const std::array surface{
        CollisionTriangle{{0, 0, -1}, {Vec3{-5, 0, 8}, Vec3{5, 0, 8}, Vec3{0, 12, 8}}}};
    MissileTarget wall{0, {0, 0, 8}, 5, 12, surface};
    wall.acquisition = MissileTarget::Acquisition{{0, 6, 8}, 2, 1.2f, 12};
    const Vec3 origin{0, 3, 0};
    CHECK(wall.pointNear(origin) == Vec3{0, 3, 8});
    CHECK(TargetAssist::select(origin, {0, 0, 1}, std::span{&wall, 1}, 30) == Vec3{0, 6, 8});
    CHECK(TargetAssist::distanceTo({0, 0, 0}, 6, wall) == Approx(8));
    WorldCollision collision;
    collision.build({surface[0]});
    CHECK(TargetAssist::select(origin, {0, 0, 1}, std::span{&wall, 1}, 30, &collision));
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 4}, Vec3{10, 0, 4}, Vec3{0, 20, 4}}}});
    CHECK_FALSE(TargetAssist::select(origin, {0, 0, 1}, std::span{&wall, 1}, 30, &collision));
}

TEST_CASE("overlapping enemy centres still select melee without a spurious aim direction",
          "[game][target-assist][melee]") {
    const std::array targets{MissileTarget{0, {0, 0, 0}, 2, 8}};
    REQUIRE(TargetAssist::around({0, 0, 0}, 6, targets, 3));
    REQUIRE_FALSE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, targets, 30));
    REQUIRE_FALSE(TargetAssist::around({0, 9, 0}, 6, targets, 3));
}

TEST_CASE("target assist chooses the nearest forward surface without targeting behind",
          "[game][target-assist]") {
    const Vec3 origin{0, 3, 0};
    const Vec3 facing{0, 0, 1};
    const std::array targets{MissileTarget{0, {3, 0, 10}, 1, 6}, MissileTarget{1, {0, 0, -1}, 1, 6},
                             MissileTarget{2, {8, 0, 2}, 1, 6}};
    REQUIRE(TargetAssist::select(origin, facing, targets, 30) == Vec3{3, 3, 10});
    REQUIRE_FALSE(TargetAssist::select(origin, facing, targets, 5));
    REQUIRE_FALSE(TargetAssist::select(origin, Vec3{0}, targets, 30));
    REQUIRE_FALSE(TargetAssist::select(origin, facing, {}, 30));
    const std::array large{targets[0], MissileTarget{3, {0, 0, 14}, 6, 6}};
    REQUIRE(TargetAssist::select(origin, facing, large, 30) == Vec3{0, 3, 14});
}

TEST_CASE("part selection uses authored weights before the same creature's body fallback",
          "[game][target-assist][garm]") {
    const Vec3 origin{0, 3, 0};
    const Vec3 forward{0, 0, 1};
    std::array targets{MissileTarget{0, {0, 0, 20}, 6, 6}, MissileTarget{0, {-2, 1, 20}, 2, 4},
                       MissileTarget{0, {2, 1, 20}, 2, 4}};
    targets[1].node = 0;
    targets[2].node = 1;
    targets[2].targetScoreScale = 7;
    REQUIRE(TargetAssist::select(origin, forward, targets, 100) == Vec3{2, 3, 20});
    std::swap(targets[0], targets[2]); // independent of body/node list ordering
    REQUIRE(TargetAssist::select(origin, forward, targets, 100) == Vec3{2, 3, 20});
    targets[0].maxTargetDistance = 10;
    REQUIRE(TargetAssist::select(origin, forward, targets, 100) == Vec3{-2, 3, 20});
    targets[1].targetScoreScale = 0;
    REQUIRE(TargetAssist::select(origin, forward, targets, 100) == Vec3{0, 3, 20});
    // Aim-disabled parts still participate in close melee and missile collision.
    REQUIRE(TargetAssist::around({-2, 0, 18}, 6, std::span{&targets[1], 1}, 3));
    CHECK(targets[1].touches({-2, 3, 20}, 1));
    const std::array rivals{targets[0], MissileTarget{1, {0, 0, 10}, 1, 6}};
    REQUIRE(TargetAssist::select(origin, forward, rivals, 100) == Vec3{0, 3, 10});
}

TEST_CASE("target assist extends range in boss encounters and rejects blocked targets",
          "[game][target-assist]") {
    const std::array target{MissileTarget{0, {0, 0, 60}, 4, 12}};
    REQUIRE_FALSE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, target, TargetAssist::kRange));
    REQUIRE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, target, TargetAssist::kBossRange));
    WorldCollision collision;
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 10}, Vec3{10, 0, 10}, Vec3{0, 20, 10}}}});
    REQUIRE_FALSE(
        TargetAssist::select({0, 3, 0}, {0, 0, 1}, target, TargetAssist::kBossRange, &collision));
}

TEST_CASE("assisted shots reach raised and lowered targets without changing horizontal pace",
          "[game][target-assist]") {
    const Vec3 origin{0, 5, 0};
    for (const f32 height : {-3.0f, 5.0f, 20.0f}) {
        const Vec3 target{6, height, 8};
        const Vec3 velocity = TargetAssist::velocity(origin, target, 20, 8);
        REQUIRE(std::hypot(velocity.x, velocity.z) == Approx(20));
        const f32 time = 0.5f;
        const Vec3 landed = origin + velocity * time - Vec3{0, 4 * time * time, 0};
        REQUIRE(glm::distance(landed, target) == Approx(0).margin(1e-5f));
    }
    REQUIRE(TargetAssist::velocity(origin, origin, 20, 8) == Vec3{0});
}
TEST_CASE("melee reach uses horizontal surfaces at any bearing, with vertical overlap and "
          "wall visibility",
          "[game][target-assist][melee]") {
    std::array targets{MissileTarget{0, {0, 0, 5}, 3, 18}, MissileTarget{1, {0, 10, 1}, 1, 4},
                       MissileTarget{2, {0, 0, -2}, 1, 4}};
    // The nearest surface wins whichever way it lies; the one overhead is out of reach.
    const auto selected = TargetAssist::around({0, 0, 0}, 6, targets, 3);
    REQUIRE(selected);
    CHECK(selected->id == 2);
    CHECK(TargetAssist::distanceTo({0, 0, 0}, 6, targets[2]) == Approx(1.0f));
    CHECK_FALSE(TargetAssist::around({0, 0, 0}, 6, targets, 1));
    targets[0] = {0, {0, 0, 5}, 1, 6};
    targets[2] = {2, {0, 0, -20}, 1, 4};
    CHECK(TargetAssist::around({0, 0, 0}, 6, targets, 6));
    WorldCollision collision;
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 2}, Vec3{10, 0, 2}, Vec3{0, 20, 2}}}});
    CHECK_FALSE(TargetAssist::around({0, 0, 0}, 6, targets, 6, &collision));
}

TEST_CASE("melee acquisition uses the requested direction without changing contact selection",
          "[game][target-assist][melee][alpha-auto-melee]") {
    const std::array targets{MissileTarget{0, {0, 0, -2}, 1, 6}, MissileTarget{1, {0, 0, 3}, 1, 6}};
    const auto ahead = TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, targets, 4, 30);
    REQUIRE(ahead);
    CHECK(ahead->id == 1);
    const auto behind = TargetAssist::ahead({0, 0, 0}, 6, {0, 0, -2}, targets, 4, 30);
    REQUIRE(behind);
    CHECK(behind->id == 0);
    const auto contact = TargetAssist::around({0, 0, 0}, 6, targets, 4);
    REQUIRE(contact);
    CHECK(contact->id == 0);
    CHECK_FALSE(TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, targets, 2, 30));
    CHECK_FALSE(TargetAssist::ahead({0, 0, 0}, 6, Vec3{0}, targets, 4, 30));
    CHECK_FALSE(TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, targets, 4, 0));
    CHECK_FALSE(TargetAssist::ahead({0, 6, 0}, 6, {0, 0, 1}, targets, 4, 30));
    WorldCollision collision;
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 1}, Vec3{10, 0, 1}, Vec3{0, 20, 1}}}});
    CHECK_FALSE(TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, targets, 4, 30, &collision));
}

TEST_CASE("the melee acquisition cone tightens over the encounter range, not the swing range",
          "[game][target-assist][melee][alpha-auto-melee]") {
    // PlayerGetTarget passes 30 normally and 200 in a boss encounter; the
    // closest_enemy/item cone interpolates from 0.707 toward 1 over that range.
    constexpr f32 kDot = 0.74f;
    const std::array targets{MissileTarget{0, {6 * std::sqrt(1 - kDot * kDot), 0, 6 * kDot}, 1, 6}};
    CHECK_FALSE(TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, targets, 10, TargetAssist::kRange));
    CHECK(TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, targets, 10, TargetAssist::kBossRange));
    const std::array close{MissileTarget{0, {2, 0, 3}, 1, 6}};
    CHECK(TargetAssist::ahead({0, 0, 0}, 6, {0, 0, 1}, close, 3, TargetAssist::kRange));
}
} // namespace
