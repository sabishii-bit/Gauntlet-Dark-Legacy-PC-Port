#include <filesystem>
#include <limits>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsTowerMotion.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/combat/MissileTarget.h"
#include "game/party/PlayerMissiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsMap floorMap() {
    formats::LegendsMap map;
    map.collision = {{.vertices = {Vec3{-20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, -20}},
                      .owner = 0,
                      .floor = true},
                     {.vertices = {Vec3{20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, 20}},
                      .owner = 0,
                      .floor = true}};
    map.starts.push_back({.position = Vec3{0, 1, 0}});
    return map;
}

void appendFloor(formats::LegendsMap& map, f32 left, f32 right, f32 near, f32 far, f32 y,
                 s32 owner) {
    map.collision.push_back(
        {.vertices = {Vec3{left, y, near}, Vec3{left, y, far}, Vec3{right, y, near}},
         .owner = owner,
         .floor = true});
    map.collision.push_back(
        {.vertices = {Vec3{right, y, near}, Vec3{left, y, far}, Vec3{right, y, far}},
         .owner = owner,
         .floor = true});
}

void appendWall(formats::LegendsMap& map, f32 z) {
    map.collision.push_back({.vertices = {Vec3{-20, 0, z}, Vec3{-20, 20, z}, Vec3{20, 0, z}},
                             .normal = {0, 0, -1},
                             .owner = 3,
                             .wall = true});
    map.collision.push_back({.vertices = {Vec3{20, 0, z}, Vec3{-20, 20, z}, Vec3{20, 20, z}},
                             .normal = {0, 0, -1},
                             .owner = 3,
                             .wall = true});
}

TEST_CASE("Legends scenery correction rechecks a door tangent against native walls and floors",
          "[legends][traversal][tower]") {
    auto map = floorMap();
    map.starts.front().position = Vec3{12, 0, 0};
    ClassStats stats;
    stats.width = 2; // A radius-one player against the native radius-nine door.
    stats.collisionY = 5;
    bool accepted = false;
    std::string_view path;
    SECTION("door tangent overlaps a wall which the original move clears") {
        path = "wall";
        appendWall(map, 6.1f);
    }
    SECTION("door tangent reaches an adjacent floor above the permitted step") {
        path = "raised floor";
        map.collision.clear();
        appendFloor(map, -20, 9, -20, 20, 0, 0);
        appendFloor(map, 9, 20, -20, 4.1f, 0, 0);
        appendFloor(map, 9, 20, 4.1f, 20, 4, 1);
    }
    SECTION("door tangent reaches the side of a gap") {
        path = "unsupported gap";
        map.collision.clear();
        appendFloor(map, -20, 8.1f, -20, 20, 0, 0);
        appendFloor(map, 8.1f, 20, -20, 4.1f, 0, 0);
    }
    SECTION("a narrow gap keeps the shared player's same-height bridge behavior") {
        path = "supported narrow gap";
        map.collision.clear();
        appendFloor(map, -20, 9, -20, 20, 0, 0);
        appendFloor(map, 9, 20, -20, 4.1f, 0, 0);
        // The endpoint is just outside this corner, but a width farther along
        // its heading reaches the left-hand face. PlayerActor intentionally
        // bridges that seam; the wider gap above has no such supporting face.
        accepted = true;
    }
    SECTION("door tangent remains on a supported unobstructed floor") {
        path = "clear floor";
        accepted = true;
    }
    INFO(path);
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{}, &stats);
    const PlayerActor before = traversal.actor();
    const Vec3 requested{8, 0, 4};
    traversal.actor().slide(requested - before.position(), &traversal.collision());
    REQUIRE(glm::distance(traversal.actor().position(), requested) < 0.001f);
    traversal.actor().turnTo(0.75f);
    const formats::LegendsN64TowerDoor door{.triggerPosition = Vec3{0}, .triggerRadius = 9};
    const Vec3 corrected = resolveLegendsTowerDoor(
        door, before.position(), requested, traversal.actor().radius(), traversal.actor().height());
    REQUIRE(corrected.x == Approx(9.2696663f).margin(0.0001f));
    REQUIRE(corrected.z == Approx(4.2943382f).margin(0.0001f));
    CHECK(traversal.correctSceneryStep(before, corrected, 1.0f / 60.0f) == accepted);
    CHECK(traversal.actor().yaw() == Approx(0.75f));
    CHECK(traversal.actor().save() == before.save());
    if (accepted) {
        CHECK(glm::distance(traversal.actor().position(), corrected) < 0.001f);
        CHECK(traversal.actor().moving());
        CHECK(traversal.actor().floorObject() == before.floorObject());
    } else {
        CHECK(traversal.actor().position() == before.position());
        CHECK(traversal.actor().floorBase() == before.floorBase());
        CHECK(traversal.actor().floorObject() == before.floorObject());
        CHECK(traversal.actor().floorFlags() == before.floorFlags());
        CHECK(traversal.actor().stepAim() == before.stepAim());
    }
}

TEST_CASE("Legends scenery correction preserves bounded descent and rejects invalid adjustments",
          "[legends][traversal][tower]") {
    LegendsTraversal traversal;
    traversal.open(floorMap(), 0, CharacterSave{});
    traversal.actor().place(Vec3{0, 1, 0});
    const PlayerActor before = traversal.actor();
    traversal.step({{0, 1}, 1}, 0, 1.0f / 60.0f);
    const Vec3 original = traversal.actor().position();
    Vec3 corrected = original;
    corrected.x = corrected.z * 0.5f;
    corrected.z *= 0.5f;
    bool accepted = false;
    std::string_view path;
    SECTION("a valid horizontal correction does not snap onto the lower floor") {
        path = "bounded descent";
        accepted = true;
    }
    SECTION("nonfinite adjustment") {
        path = "nonfinite";
        corrected.x = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("vertical adjustment") {
        path = "vertical";
        corrected.y += 2;
    }
    SECTION("a correction cannot lengthen the movement") {
        path = "lengthening";
        corrected.z += 10;
    }
    INFO(path);
    CHECK(traversal.correctSceneryStep(before, corrected, 1.0f / 60.0f) == accepted);
    if (accepted) {
        CHECK(traversal.actor().position().y == Approx(original.y));
        CHECK(traversal.actor().position().y > 0);
        CHECK(traversal.actor().floorBase() == Approx(0));
        CHECK(traversal.actor().floorObject() == before.floorObject());
    } else {
        CHECK(traversal.actor().position() == before.position());
        CHECK(traversal.actor().floorBase() == before.floorBase());
    }
}

TEST_CASE("Legends traversal uses native collision and a private selected character",
          "[legends][traversal]") {
    auto map = floorMap();
    CharacterSave save;
    save.name = "NATIVE";
    save.gold = 123;
    LegendsTraversal traversal;
    traversal.open(map, 2, save);
    REQUIRE(traversal.authoredStart());
    CHECK(traversal.actor().player() == 2);
    CHECK(traversal.actor().position() == Vec3{0});
    CHECK(traversal.actor().save() == save);
    const MoveInput forward{{0, 1}, 1};
    for (s32 tick = 0; tick < 60; ++tick) {
        traversal.step(forward, 0, 1.0f / 60.0f);
    }
    CHECK(traversal.actor().position().z == Approx(traversal.actor().speed()).margin(0.01));
    CHECK(traversal.actor().position().y == Approx(0));
    CHECK(traversal.actor().save() == save);
    CHECK(save.gold == 123);
}

TEST_CASE("Imported scenery motion correction never rewinds gameplay awards or item use",
          "[legends][traversal][imported-level]") {
    LegendsTraversal traversal;
    traversal.open(floorMap(), 0, CharacterSave{});
    const PlayerActor before = traversal.actor();
    traversal.step({{0, 1}, 1}, 0, 1.0f / 60);
    auto& actor = traversal.actor();
    actor.save().gold = 123;
    actor.save().progress().health = 321;
    actor.save().progress().inventory.keys = 2;
    const CharacterSave afterEvents = actor.save();
    Vec3 corrected = actor.position();
    bool valid = false;
    SECTION("a valid shortened step") {
        corrected.z *= .5f;
        valid = true;
    }
    SECTION("an invalid correction") {
        corrected.y += 2;
    }
    CHECK(correctImportedSceneryStep(actor, before, traversal.collision(), corrected, 1.0f / 60) ==
          valid);
    CHECK(actor.save() == afterEvents);
}

TEST_CASE("Legends traversal refuses visual meshes as fabricated floor and keeps its prior map",
          "[legends][traversal]") {
    LegendsTraversal traversal;
    traversal.open(floorMap(), 0, CharacterSave{});
    auto invalid = floorMap();
    SECTION("no native collision") {
        invalid.collision.clear();
    }
    SECTION("no floor") {
        for (auto& triangle : invalid.collision) {
            triangle.floor = false;
            triangle.wall = true;
        }
    }
    SECTION("nonfinite collision") {
        invalid.collision[0].vertices[0].x = std::numeric_limits<f32>::infinity();
    }
    SECTION("no normal") {
        invalid.collision[0].normal = Vec3{0};
    }
    SECTION("nonfinite start") {
        invalid.starts[0].position.x = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("too many triangle-to-cell references") {
        const f32 span = WorldCollision::kCellSize * 200.0f;
        auto triangle = invalid.collision.front();
        triangle.vertices = {Vec3{0}, Vec3{0, 0, span}, Vec3{span, 0, 0}};
        invalid.collision.assign(250, triangle);
    }
    const auto before = traversal.actor().checkpoint();
    CHECK_THROWS_AS(traversal.open(invalid, 0, CharacterSave{}), FormatError);
    CHECK(traversal.actor().checkpoint() == before);
    CHECK(traversal.collision().loaded());
}

TEST_CASE("Legends traversal discloses a real-floor fallback when a map lacks arrival markers",
          "[legends][traversal]") {
    auto map = floorMap();
    map.starts.clear();
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    CHECK_FALSE(traversal.authoredStart());
    CHECK(traversal.collision().floorAt(traversal.start(), 0.1f, 0.1f).has_value());
}

TEST_CASE("Legends traversal blocks walls and unsupported ledges", "[legends][traversal]") {
    auto map = floorMap();
    SECTION("wall") {
        map.collision.push_back({.vertices = {Vec3{-20, 0, 3}, Vec3{-20, 20, 3}, Vec3{20, 0, 3}},
                                 .normal = {0, 0, -1},
                                 .owner = 1,
                                 .wall = true});
        map.collision.push_back({.vertices = {Vec3{20, 0, 3}, Vec3{-20, 20, 3}, Vec3{20, 20, 3}},
                                 .normal = {0, 0, -1},
                                 .owner = 1,
                                 .wall = true});
        LegendsTraversal traversal;
        traversal.open(map, 0, CharacterSave{});
        for (s32 tick = 0; tick < 240; ++tick) {
            traversal.step({{0, 1}, 1}, 0, 1.0f / 60.0f);
        }
        CHECK(traversal.actor().position().z <= 3 - traversal.actor().radius() + 0.01f);
        CHECK(traversal.actor().position().z > 1);
    }
    SECTION("ledge") {
        LegendsTraversal traversal;
        traversal.open(map, 0, CharacterSave{});
        for (s32 tick = 0; tick < 600; ++tick) {
            traversal.step({{0, 1}, 1}, 0, 1.0f / 60.0f);
        }
        CHECK(traversal.actor().position().z <= 20.1f);
        CHECK(traversal.actor().position().z > 10);
        CHECK(traversal.actor().position().y == Approx(0));
    }
}

TEST_CASE("Legends traversal can enter every extracted map with native collision",
          "[legends][traversal][assets]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    const auto survey = [&](const std::filesystem::path& levels, bool n64, usize expected) {
        if (!std::filesystem::is_directory(levels)) {
            SKIP("Legends maps are not installed");
        }
        usize maps = 0;
        for (const auto& entry : std::filesystem::directory_iterator(levels)) {
            const auto world = entry.path() / (n64 ? "worlds.wad" : "worlds.rom");
            const auto models = entry.path() / (n64 ? "objects.wad" : "objects.rom");
            if (!std::filesystem::is_regular_file(world) ||
                !std::filesystem::is_regular_file(models) ||
                std::filesystem::file_size(world) == 0) {
                continue;
            }
            INFO(entry.path().string());
            const auto ipr = entry.path() / "ipr.rom";
            const auto placementBytes =
                n64 && std::filesystem::is_regular_file(ipr) ? readFile(ipr) : std::vector<u8>{};
            const auto map =
                n64 ? formats::LegendsMap::parseN64(readFile(world), readFile(models),
                                                    placementBytes)
                    : formats::LegendsMap::parseDreamcast(readFile(world), readFile(models));
            LegendsTraversal traversal;
            REQUIRE_NOTHROW(traversal.open(map, 0, CharacterSave{}));
            CHECK(traversal.collision().loaded());
            for (s32 tick = 0; tick < 180; ++tick) {
                const auto before = traversal.actor().position();
                traversal.step({{0, 1}, 1}, 0, 1.0f / 60.0f);
                const auto position = traversal.actor().position();
                INFO("tick " << tick << " authored " << traversal.authoredStart() << " from "
                             << before.x << ',' << before.y << ',' << before.z << " to "
                             << position.x << ',' << position.y << ',' << position.z << " floor "
                             << traversal.actor().floorObject());
                REQUIRE(std::isfinite(position.y));
                CHECK(glm::distance(Vec2{before.x, before.z}, Vec2{position.x, position.z}) < 0.3f);
            }
            ++maps;
        }
        CHECK(maps == expected);
    };
    SECTION("N64") {
        survey(assets / "n64/extracted/levels", true, 34);
    }
    SECTION("Dreamcast") {
        survey(assets / "dreamcast/extracted/native/levels", false, 41);
    }
}

TEST_CASE("N64 body barriers keep characters in bounds without stopping projectiles",
          "[legends][traversal][missiles][ravine]") {
    auto map = floorMap();
    appendWall(map, 3);
    bool native = true;
    u32 channels = 0x200;
    bool blocksShot = false;
    SECTION("native body-only ravine") {}
    SECTION("native ordinary wall") {
        channels = 0x20;
        blocksShot = true;
    }
    SECTION("native dual-channel wall") {
        channels = 0x220;
        blocksShot = true;
    }
    SECTION("unrelated platform flags are not N64 channels") {
        native = false;
        blocksShot = true;
    }
    map.objects.resize(4);
    map.objects.front().hasNativeDrawState = native;
    for (auto& triangle : map.collision) {
        if (triangle.wall) {
            triangle.flags = channels;
        }
    }
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    const Vec3 from{0, 5, 0};
    const Vec3 to{0, 5, 6};
    CHECK(traversal.collision().sweepSphere(from, to, .25f).has_value());
    CHECK(traversal.collision()
              .sweepSphere(from, to, .25f, WorldCollision::kMissileQueryFlags)
              .has_value() == blocksShot);
    MissileSpec spec;
    spec.radius = .25f;
    spec.weight = 0;
    PlayerMissiles shots;
    MissileLaunch launch;
    launch.spec = &spec;
    launch.position = from;
    launch.velocity = Vec3{0, 0, 30};
    REQUIRE(shots.launch(launch));
    for (u32 tick = 0; tick < 12; ++tick) {
        shots.update(1.0f / 60, &traversal.collision());
    }
    CHECK((shots.count() == 0) == blocksShot);
    CHECK(shots.takeImpacts().empty() == !blocksShot);
}

TEST_CASE("Mountain ravine projectiles reach targets across native body-only barriers",
          "[legends][traversal][missiles][ravine][assets]") {
    std::string_view stage;
    Vec3 from;
    Vec3 to;
    SECTION("mount4 FL12 to FL15 beside NS_LAVAFLOW__NC01") {
        stage = "mount4";
        from = {-59, 5, -26};
        to = {-83, 5, -26};
    }
    SECTION("mount5 ravine wall CTRI 2097") {
        stage = "mount5";
        from = {-43.27204f, 7.145833f, -44.095375f};
        to = {-30.457127f, 7.145833f, -53.675457f};
    }
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / stage;
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 map is not installed");
    }
    const auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                   readFile(directory / "objects.wad"),
                                                   readFile(directory / "ipr.rom"));
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    const auto& collision = traversal.collision();
    REQUIRE(collision.sweepSphere(from, to, .2f).has_value());
    REQUIRE_FALSE(collision.sweepSphere(from, to, .2f, WorldCollision::kMissileQueryFlags));
    MissileSpec spec;
    spec.radius = .2f;
    spec.weight = 0;
    PlayerMissiles shots;
    MissileLaunch launch;
    launch.spec = &spec;
    launch.position = from;
    launch.velocity = glm::normalize(to - from) * 30.0f;
    REQUIRE(shots.launch(launch));
    const std::array targets{MissileTarget{77, to - Vec3{0, 1, 0}, .5f, 2}};
    for (u32 tick = 0; tick < 120 && shots.count() != 0; ++tick) {
        shots.update(1.0f / 60, &collision, targets);
    }
    const auto hits = shots.takeImpacts();
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().target == 77);
}

TEST_CASE("Legends traversal refuses the oversized slide at a low wall's finite edge",
          "[legends][traversal]") {
    auto map = floorMap();
    map.collision.push_back({.vertices = {Vec3{-20, 0, 3}, Vec3{-20, 2.25f, 3}, Vec3{20, 2.25f, 3}},
                             .normal = {0, 0, -1},
                             .owner = 1,
                             .wall = true});
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    for (s32 tick = 0; tick < 180; ++tick) {
        const Vec3 before = traversal.actor().position();
        traversal.step({{0, 1}, 1}, 0, 1.0f / 60.0f);
        const Vec3 after = traversal.actor().position();
        CHECK(after.z >= before.z - 0.001f);
        CHECK(after.z - before.z <= traversal.actor().speed() / 60.0f + 0.001f);
    }
    CHECK(traversal.actor().position().z > 1.5f);
    CHECK(traversal.actor().position().z < 3);
    // Releasing the blocked direction must still allow walking away.
    const f32 blocked = traversal.actor().position().z;
    traversal.step({{0, -1}, 1}, 0, 1.0f / 60.0f);
    CHECK(traversal.actor().position().z < blocked);
}
} // namespace
