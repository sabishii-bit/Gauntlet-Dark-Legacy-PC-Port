#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "expansions/shared/scenes/LegendsTraversal.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
std::filesystem::path mount2() {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount2";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Legends extraction unavailable");
    }
    return directory;
}
formats::LegendsMap nativeMap(const std::filesystem::path& directory) {
    return formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                         readFile(directory / "objects.wad"),
                                         readFile(directory / "ipr.rom"));
}
} // namespace

TEST_CASE("Dagger Peak shootwalls reveal all three native pressure switches and release collision",
          "[legends][native-scenery][switches][scene-adapter][assets]") {
    const auto directory = mount2();
    auto map = nativeMap(directory);
    REQUIRE(map.destructibles.size() == 4);
    const auto root = directory.parent_path().parent_path();
    const auto switches = formats::decodeLegendsN64Switches(
        readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
    REQUIRE(switches.switches.size() == 3);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    LegendsTraversal probe;
    probe.open(map, 0, CharacterSave{});
    auto collision = probe.collision();
    scene->bindCollision(collision);
    for (const auto& plate : switches.switches) {
        const auto wall = std::ranges::find_if(map.destructibles, [&](const auto& value) {
            return glm::all(glm::greaterThanEqual(plate.position, value.minimum)) &&
                   glm::all(glm::lessThanEqual(plate.position, value.maximum));
        });
        REQUIRE(wall != map.destructibles.end());
        INFO(map.objects[wall->object].name);
        REQUIRE(scene->worldObjectVisible(wall->object));
        REQUIRE(collision.solid(static_cast<s32>(wall->object)));
        const auto faces = std::ranges::count_if(map.collision, [&](const auto& face) {
            return face.owner == static_cast<s32>(wall->object) && (face.flags & 0x800U) != 0;
        });
        REQUIRE(faces > 0);
        // A normal physical hit damages, but does not erase a 30HP wall.
        REQUIRE(scene->strikeWorldObject(static_cast<s32>(wall->object), 1, 0));
        CHECK(scene->worldObjectWhiteBlend(wall->object) == 1);
        CHECK(scene->worldObjectVisible(wall->object));
        REQUIRE(scene->strikeWorldObject(static_cast<s32>(wall->object), 1000, 0));
        scene->syncCollision(collision);
        CHECK_FALSE(scene->worldObjectVisible(wall->object));
        CHECK(scene->worldObjectWhiteBlend(wall->object) == 0);
        CHECK_FALSE(collision.solid(static_cast<s32>(wall->object)));
        CHECK_FALSE(scene->strikeWorldObject(static_cast<s32>(wall->object), 1000, 0));
        const auto target =
            std::ranges::find(map.objects, plate.target, &formats::LegendsMapObject::name);
        REQUIRE(target != map.objects.end());
        const auto targetIndex = static_cast<usize>(target - map.objects.begin());
        formats::LegendsN64Fixture fixture;
        fixture.position = plate.position;
        fixture.groundProjected = plate.groundProjected;
        const auto position = formats::legendsN64FixturePosition(fixture, map.collision);
        const std::array visitors{LegendsPortalVisitor{0, position}};
        scene->touchSwitches(visitors);
        for (s32 tick = 0; tick < 180; ++tick) {
            scene->update(1.0f / 60);
        }
        CHECK(scene->worldObjectOffset(targetIndex).y ==
              Approx(static_cast<f32>(plate.secondEndpoint) * .1f));
    }
}

TEST_CASE("Native surviving shootwalls submit a transient white material flash",
          "[legends][native-scenery][assets]") {
    const auto directory = mount2();
    auto map = nativeMap(directory);
    REQUIRE_FALSE(map.destructibles.empty());
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map,
                 LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map));
    const auto object = map.destructibles.front().object;
    const auto flashes = [&] {
        device.draws.clear();
        preview.drawWorld(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::All, 1);
        return std::ranges::count_if(device.draws,
                                     [](const auto& draw) { return draw.state.whiteBlend > 0; });
    };
    REQUIRE(flashes() == 0);
    REQUIRE(preview.adapter()->strikeWorldObject(static_cast<s32>(object), 1, 0));
    CHECK(flashes() > 0);
    preview.adapter()->update(1.0f / 30);
    CHECK(flashes() == 0);
}

TEST_CASE("Dagger Peak obelisk statue retains native steep moving-wall contacts",
          "[legends][native-scenery][collision][assets]") {
    auto map = nativeMap(mount2());
    LegendsTraversal probe;
    probe.open(map, 0, CharacterSave{});
    auto collision = probe.collision();
    const auto face =
        std::ranges::find(map.collision, 817U, &formats::LegendsMapCollisionTriangle::nativeIndex);
    REQUIRE(face != map.collision.end());
    REQUIRE(face->owner >= 0);
    REQUIRE(map.objects[static_cast<usize>(face->owner)].name == "ELEV1");
    REQUIRE(face->wall);
    REQUIRE(face->normal.y < -.866f);
    const Vec3 centre = (face->vertices[0] + face->vertices[1] + face->vertices[2]) / 3.0f;
    const auto hit =
        collision.sweepSphere(centre + face->normal * 3.0f, centre - face->normal, .5f);
    REQUIRE(hit);
    CHECK(hit->object == face->owner);
    const std::array owners{face->owner};
    collision.setMovingObjects(owners, 0x1000);
    collision.setObjectTransform(face->owner, glm::translate(Mat4{1}, Vec3{0, -4, 0}));
    const Vec3 moved = centre + Vec3{0, -4, 0};
    const auto movingHit =
        collision.sweepSphere(moved + face->normal * 3.0f, moved - face->normal, .5f);
    REQUIRE(movingHit);
    CHECK(movingHit->object == face->owner);

    // Exercise the common player's wall sweep, not only a ray through the face.
    // A horizontal approach meets the underside while standing on the ground
    // below it; the actor must retain the authored object contact.
    PlayerActor player;
    const Vec3 horizontal = glm::normalize(Vec3{face->normal.x, 0, face->normal.z});
    const Vec3 from = centre + horizontal * 6.0f - Vec3{0, 3.5f, 0};
    player.spawn(0, CharacterSave{}, nullptr, from, 0);
    player.slide(-horizontal * 8.0f, &probe.collision());
    CHECK(std::ranges::any_of(player.wallContacts(),
                              [&](const auto& contact) { return contact.object == face->owner; }));

    const auto top =
        std::ranges::find(map.collision, 826U, &formats::LegendsMapCollisionTriangle::nativeIndex);
    REQUIRE(top != map.collision.end());
    REQUIRE(top->floor);
    REQUIRE_FALSE(top->wall);
    const Vec3 topCentre = (top->vertices[0] + top->vertices[1] + top->vertices[2]) / 3.0f;
    const auto support = probe.collision().floorAt(topCentre + Vec3{0, .25f, 0}, 0, 1);
    REQUIRE(support);
    CHECK(support->object == face->owner);
    CHECK(support->y == Approx(topCentre.y).margin(.001f));
}

TEST_CASE("Native wall channels do not turn flat walkable tops into movement barriers",
          "[legends][native-scenery][collision]") {
    formats::LegendsMap map;
    map.objects.resize(1);
    map.objects[0].hasNativeDrawState = true;
    map.starts.push_back({.position = {-5, 0, 0}});
    map.collision = {{.vertices = {Vec3{-20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, -20}},
                      .owner = 0,
                      .floor = true},
                     {.vertices = {Vec3{20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, 20}},
                      .owner = 0,
                      .floor = true}};
    SECTION("floor-only native neighbor remains unchanged") {}
    SECTION("a top with both native query channels remains walkable") {
        for (auto& face : map.collision) {
            face.wall = true;
        }
    }
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    for (s32 tick = 0; tick < 120; ++tick) {
        traversal.step({{1, 0}, 1}, 0, 1.0f / 60);
    }
    CHECK(traversal.actor().position().x > 4);
    CHECK(traversal.actor().position().y == 0);
    CHECK(traversal.actor().wallContacts().empty());
}
