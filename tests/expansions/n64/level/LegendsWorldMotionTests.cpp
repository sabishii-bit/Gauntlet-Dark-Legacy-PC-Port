#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/level/LegendsWorldMotion.h"
#include "game/level/LevelWorld.h"
#include "game/party/FloorRiding.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsMap motionMap() {
    formats::LegendsMap map;
    map.minBounds = {-5, 0, -5};
    map.maxBounds = {15, 25, 5};
    map.objects.resize(3);
    map.objects[0].name = "ELEV1";
    map.objects[0].flags = 0x8000;
    map.objects[1].name = "GROUND";
    map.objects[2].name = "LIFT_DECORATION";
    map.objects[2].parent = 0;
    map.collision.push_back({.vertices = {{{-5, 20, -5}, {0, 20, 5}, {5, 20, -5}}},
                             .normal = {0, 1, 0},
                             .owner = 0,
                             .floor = true});
    map.collision.push_back({.vertices = {{{5, 0, -5}, {10, 0, 5}, {15, 0, -5}}},
                             .normal = {0, 1, 0},
                             .owner = 1,
                             .floor = true});
    // Child drawables inherit movement, but native collision does not inherit
    // a parent's motion record (e003faf4 uses the triangle owner's own slot).
    map.collision.push_back({.vertices = {{{25, 20, -5}, {30, 20, 5}, {35, 20, -5}}},
                             .normal = {0, 1, 0},
                             .owner = 2,
                             .floor = true});
    return map;
}

formats::LegendsN64Switch lift() {
    formats::LegendsN64Switch plate;
    plate.nativeIndex = 9;
    plate.flags = 0x3802;
    plate.activationFlags = 0x02000000;
    plate.target = "ELEV1";
    plate.position = {10, 0, 0};
    plate.firstEndpoint = -40;
    plate.secondEndpoint = 0;
    plate.supported = true;
    return plate;
}

WorldCollision collisionOf(const formats::LegendsMap& map) {
    std::vector<CollisionTriangle> triangles;
    triangles.reserve(map.collision.size());
    for (const auto& source : map.collision) {
        triangles.push_back({source.normal, source.vertices, source.owner, 4});
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    return collision;
}

class MovingImported final : public ImportedLevel {
public:
    explicit MovingImported(const formats::LegendsMap& map) : motion(map, std::array{lift()}) {}
    LegendsWorldMotion motion;
    void capturePresentation() override { motion.capturePresentation(); }
    void update(f32 seconds, bool /*timeStopped*/) override { motion.update(seconds); }
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    void bindCollision(WorldCollision& collision) const override {
        motion.bindCollision(collision);
    }
    void syncCollision(WorldCollision& collision) const override {
        motion.syncCollision(collision);
    }
};
} // namespace

TEST_CASE("Native moving worlds share collision and common rider tick placement",
          "[legends][legends-world-motion]") {
    const auto map = motionMap();
    auto imported = std::make_shared<MovingImported>(map);
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    auto layout = WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds);
    const bool loaded =
        world.loadImported(device, {}, ref, std::move(layout), collisionOf(map), imported);
    REQUIRE(loaded);
    REQUIRE(world.collision().moving(0));
    CHECK_FALSE(world.collision().moving(1));
    CHECK_FALSE(world.collision().moving(2));
    CHECK(world.collision().triangleCount() == 3);
    auto hit = world.collision().floorAt({0, 16, 0}, 1, 1);
    REQUIRE(hit);
    CHECK(hit->y == 16);
    CHECK((hit->objectFlags & FloorRiding::kMoving) != 0);
    CHECK(imported->motion.offset(0).y == -4);
    CHECK(imported->motion.offset(2).y == -4);
    REQUIRE(world.collision().floorAt({30, 20, 0}, 1, 1));

    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 16, 0}, 0);
    FloorRiding::land(players, 0, players[0].actor.position(), world.collision());
    REQUIRE(players[0].floor.object == 0);
    REQUIRE(players[0].floor.placement);
    const std::array visitors{LegendsPortalVisitor{0, {10, .2f, 0}, true, .5f, 3}};
    imported->motion.touch(visitors);
    REQUIRE(imported->motion.activated(9));
    world.capturePresentation();
    world.update(.25f, true); // Stop Time does not freeze ordinary moving scenery.
    FloorRiding::carry(players[0], world.collision());
    CHECK(players[0].actor.position().y == Approx(17));
    CHECK(imported->motion.offset(0, 0).y == -4);
    CHECK(imported->motion.offset(0, .5f).y == -3.5f);
    CHECK(imported->motion.offset(0, 1).y == -3);
    hit = world.collision().floorAt({0, 17, 0}, 1, 1);
    REQUIRE(hit);
    CHECK(hit->y == 17);
    FloorRiding::carry(players[0], world.collision());
    CHECK(players[0].actor.position().y == Approx(17));
    world.update(1, false);
    FloorRiding::carry(players[0], world.collision());
    CHECK(players[0].actor.position().y == Approx(20));
    CHECK_FALSE(imported->motion.motion().targets()[0].moving);
    CHECK(world.collision().triangleCount() == 3);
}

TEST_CASE("Native contact plates ride only their selected floor owner",
          "[legends][legends-world-motion]") {
    const auto map = motionMap();
    auto plate = lift();
    plate.position = {0, 20, 0};
    LegendsWorldMotion motion{map, std::array{plate}};
    REQUIRE(motion.platePosition(9));
    CHECK(motion.platePosition(9)->y == Approx(16.2f));
    motion.touch(std::array{LegendsPortalVisitor{0, {0, 16, 0}, true, .5f, 3}});
    CHECK(motion.activated(9));
    motion.update(.5f);
    CHECK(motion.platePosition(9)->y == Approx(18.2f));
    plate.groundProjected = false;
    const LegendsWorldMotion mounted{map, std::array{plate}};
    CHECK(mounted.platePosition(9)->y == 20);
    CHECK_FALSE(mounted.platePosition(999));
}

TEST_CASE("Native motion retains one moving cue per target and ends it exactly at the goal",
          "[legends][legends-world-motion][motion-sounds]") {
    const auto map = motionMap();
    auto forward = lift();
    forward.flags = 0x2a02; // Request the second endpoint, without a one-shot plate.
    auto backward = forward;
    backward.nativeIndex = 10;
    backward.flags = 0x2902; // Request the first endpoint on the same native target.
    LegendsWorldMotion motion{map, std::array{forward, backward}};
    REQUIRE(motion.motion().targets().size() == 1);
    CHECK(motion.soundLoops().empty());
    CHECK(motion.takeSounds().empty());
    REQUIRE(motion.activate(9));
    CHECK(motion.soundLoops().empty()); // A press is not actual movement.
    motion.update(.25f);
    auto loops = motion.soundLoops();
    REQUIRE(loops.size() == 1);
    CHECK(loops[0].id == 0);
    CHECK(loops[0].token == 0x1fe);
    CHECK(loops[0].position == Vec3{0, -3, 0});
    CHECK(motion.takeSounds().empty());
    motion.capturePresentation();
    motion.update(0);
    motion.update(std::numeric_limits<f32>::quiet_NaN());
    CHECK(motion.soundLoops().size() == 1);
    CHECK(motion.takeSounds().empty());
    motion.update(.75f);
    CHECK(motion.soundLoops().empty());
    auto ended = motion.takeSounds();
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].token == 0x1ff);
    CHECK(ended[0].position == Vec3{0});
    motion.update(.25f);
    CHECK(motion.takeSounds().empty());

    // A later reverse request owns a new lifetime, not an indefinitely
    // repeating endpoint cue or a second sound for the same shared target.
    REQUIRE(motion.activate(10));
    motion.update(.25f);
    loops = motion.soundLoops();
    REQUIRE(loops.size() == 1);
    CHECK(loops[0].id == 0);
    CHECK(loops[0].position == Vec3{0, -1, 0});
    motion.update(.75f);
    CHECK(motion.soundLoops().empty());
    ended = motion.takeSounds();
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].position == Vec3{0, -4, 0});
    CHECK(motion.takeSounds().empty());
}

TEST_CASE("Native zero-distance motion does not fabricate a movement or endpoint sound",
          "[legends][legends-world-motion][motion-sounds]") {
    auto plate = lift();
    plate.firstEndpoint = 0;
    LegendsWorldMotion motion{motionMap(), std::array{plate}};
    REQUIRE(motion.activate(9));
    motion.update(1);
    CHECK(motion.soundLoops().empty());
    CHECK(motion.takeSounds().empty());
}

TEST_CASE("Native decorative motion does not invent collision movement",
          "[legends][legends-world-motion]") {
    auto map = motionMap();
    map.objects[0].flags = 0;
    auto plate = lift();
    plate.position = {0, 20, 0};
    const LegendsWorldMotion motion{map, std::array{plate}};
    auto collision = collisionOf(map);
    motion.bindCollision(collision);
    CHECK_FALSE(collision.moving(0));
    CHECK(motion.offset(0).y == -4);
    CHECK(motion.platePosition(9)->y == Approx(16.2f));
    const auto hit = collision.floorAt({0, 20, 0}, 1, 1);
    REQUIRE(hit);
    CHECK(hit->y == 20);
}

TEST_CASE("Native motion refuses missing ownership and malformed hierarchy",
          "[legends][legends-world-motion]") {
    auto map = motionMap();
    auto plate = lift();
    SECTION("missing target") {
        plate.target = "ABSENT";
    }
    SECTION("prefix without native separator") {
        map.objects[0].name = "ELEV10";
    }
    SECTION("invalid parent") {
        map.objects[0].parent = 99;
        CHECK_THROWS_AS((LegendsWorldMotion{map, std::array{plate}}), std::invalid_argument);
        return;
    }
    SECTION("cycle") {
        map.objects[0].parent = 2;
        CHECK_THROWS_AS((LegendsWorldMotion{map, std::array{plate}}), std::invalid_argument);
        return;
    }
    const LegendsWorldMotion motion{map, std::array{plate}};
    CHECK(motion.motion().targets().empty());
    CHECK_FALSE(motion.warnings().empty());
    CHECK(motion.offset(999) == Vec3{0});
}

TEST_CASE("Native target lookup prefers exact names then the first authored subtree match",
          "[legends][legends-world-motion]") {
    auto map = motionMap();
    map.objects[0].name = "ELEV1#B";
    map.objects[0].nativeOrder = 2;
    map.objects[1].name = "ELEV1_A";
    map.objects[1].nativeOrder = 0;
    map.objects[2].parent = -1;
    const auto plate = lift();
    const LegendsWorldMotion prefix{map, std::array{plate}};
    CHECK(prefix.warnings().empty());
    CHECK(prefix.offset(0) == Vec3{0});
    CHECK(prefix.offset(1).y == -4);
    map.objects[0].name = "ELEV1";
    const LegendsWorldMotion exact{map, std::array{plate}};
    CHECK(exact.offset(0).y == -4);
    CHECK(exact.offset(1) == Vec3{0});
    map.objects[1].name = "ELEV1";
    const LegendsWorldMotion duplicate{map, std::array{plate}};
    CHECK(duplicate.offset(0) == Vec3{0});
    CHECK(duplicate.offset(1).y == -4);
}

TEST_CASE("Native Mountain lift collision follows the normalized drawable displacement",
          "[legends][legends-world-motion][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/mount1";
    if (!std::filesystem::exists(directory / "worlds.wad")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                   readFile(directory / "objects.wad"));
    const auto definitions = formats::decodeLegendsN64Switches(
        readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
    LegendsWorldMotion motion{map, definitions.switches};
    REQUIRE(motion.warnings().empty());
    REQUIRE(motion.motion().targets().size() == 2);
    auto collision = collisionOf(map);
    const auto object =
        std::ranges::find(map.objects, std::string{"ELEV1"}, &formats::LegendsMapObject::name);
    REQUIRE(object != map.objects.end());
    const auto id = static_cast<s32>(object - map.objects.begin());
    motion.bindCollision(collision);
    REQUIRE(collision.moving(id));
    const auto top = std::ranges::find_if(map.collision, [&](const auto& triangle) {
        return triangle.owner == id && triangle.floor && triangle.normal.y > .99f;
    });
    REQUIRE(top != map.collision.end());
    const Vec3 centre = (top->vertices[0] + top->vertices[1] + top->vertices[2]) / 3.0f;
    auto floor = collision.floorAt(centre + Vec3{0, -10, 0}, .02f, .02f);
    REQUIRE(floor);
    CHECK(floor->object == id);
    CHECK(floor->y == Approx(centre.y - 10).margin(.001f));
    CHECK((*collision.objectTransform(id))[3].y == -10);
    CHECK(motion.offset(static_cast<usize>(id)).y == -10);
    // The native authored mesh/collision bounds agree before motion. Do not
    // apply the internal half-drawable delta again in imported coordinates.
    REQUIRE(motion.activate(2));
    motion.capturePresentation();
    motion.update(1);
    motion.syncCollision(collision);
    CHECK((*collision.objectTransform(id))[3].y == -6);
    CHECK(motion.offset(static_cast<usize>(id)).y == -6);
    CHECK(motion.offset(static_cast<usize>(id), .5f).y == -8);
    floor = collision.floorAt(centre + Vec3{0, -6, 0}, .02f, .02f);
    REQUIRE(floor);
    CHECK(floor->object == id);
    CHECK(floor->y == Approx(centre.y - 6).margin(.001f));
    CHECK(collision.triangleCount() == map.collision.size());
}

TEST_CASE("All supported native N64 switch targets bind without guessed name grouping",
          "[legends][legends-world-motion][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "items/generic/ptrap.rom")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto templates = readFile(root / "items/generic/ptrap.rom");
    usize admitted = 0;
    usize maps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path());
        const auto map = formats::LegendsMap::parseN64(readFile(entry.path() / "worlds.wad"),
                                                       readFile(entry.path() / "objects.wad"));
        const auto definitions =
            formats::decodeLegendsN64Switches(readFile(entry.path() / "ipr.rom"), templates);
        const LegendsWorldMotion motion{map, definitions.switches, 4};
        CHECK(motion.warnings().empty());
        admitted += motion.motion().switches().size();
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(admitted == 473);
}
