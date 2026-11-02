#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBosses.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/level/MissileStop.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct ArenaFixture {
    test::FakeRenderDevice device;
    LegendsBossEncounter encounter;
    formats::LegendsMap map;
    std::unique_ptr<LegendsSceneAdapter> scene;

    explicit ArenaFixture(std::string_view name) {
        const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
        const auto directory = root / "levels" / name;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto decoded = legendsN64Boss(name, readFile(root / "_program/game.bin"),
                                            readFile(root / "_program/Boss.bin"));
        REQUIRE(decoded);
        encounter = *decoded;
        const auto models = readFile(directory / "objects.wad");
        map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                            readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    }

    std::vector<ImmediateVertex> geometryNear(Vec3 centre) {
        device.draws.clear();
        const auto camera = CameraFrame::of(WorldCamera{});
        scene->draw(device, Mat4{1}, camera, TreeModel::Pass::Opaque);
        scene->draw(device, Mat4{1}, camera, TreeModel::Pass::Blended);
        std::vector<ImmediateVertex> vertices;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                if (glm::distance(Vec2{vertex.position.x, vertex.position.z},
                                  Vec2{centre.x, centre.z}) < 5) {
                    vertices.push_back(vertex);
                }
            }
        }
        return vertices;
    }
};
} // namespace

TEST_CASE("Imported Skorne arena starts with its native intact cap and permanent floor",
          "[legends][arena][world-presentation][assets]") {
    ArenaFixture fixture{"boss2"};
    usize fragments = 0;
    usize floors = 0;
    for (usize index = 0; index < fixture.map.objects.size(); ++index) {
        const auto& object = fixture.map.objects[index];
        INFO(object.name);
        if ((object.flags & 0x200U) != 0) {
            ++fragments;
            REQUIRE(object.name.starts_with("ROCKFLY"));
            CHECK(fixture.scene->worldObjectVisible(index));
            CHECK(fixture.scene->worldObjectTransform(index) == Mat4{1});
        } else if (object.name.starts_with("FLOOR__FL")) {
            ++floors;
            CHECK(fixture.scene->worldObjectVisible(index));
        }
    }
    REQUIRE(fragments == 43);
    REQUIRE(floors == 2);
    // The compatibility entrance cue, not an inferred loading clock, releases
    // the authored cap. Waiting before the encounter preserves the intact floor.
    fixture.scene->update(1.0f);
    for (usize index = 0; index < fixture.map.objects.size(); ++index) {
        if ((fixture.map.objects[index].flags & 0x200U) != 0) {
            CHECK(fixture.scene->worldObjectVisible(index));
            CHECK(fixture.scene->worldObjectTransform(index) == Mat4{1});
        }
    }
}

TEST_CASE("Imported Skorne entrance animates only authored fragments and keeps walkable floors",
          "[legends][arena][boss-eruption][assets]") {
    ArenaFixture fixture{"boss2"};
    LegendsTraversal traversal;
    traversal.open(fixture.map, 0, CharacterSave{});
    auto collision = traversal.collision();
    fixture.scene->bindCollision(collision);
    const usize triangles = collision.triangleCount();
    std::vector<std::pair<Vec3, FloorHit>> floors;
    // boss2's 116 GRID-referenced floor-channel triangles have no F000 owner
    // marker: their owner is -1, independently of the FLOOR__FL render meshes.
    // Exercise the actual permanent walkable surfaces, not guessed render IDs.
    for (const auto& face : fixture.map.collision) {
        if (!face.floor || face.normal.y < WorldCollision::kFloorNormalY) {
            continue;
        }
        REQUIRE(face.owner == -1);
        const Vec3 point = (face.vertices[0] + face.vertices[1] + face.vertices[2]) / 3.0f;
        const auto floor = collision.floorAt(point, .5f, .5f);
        REQUIRE(floor);
        floors.emplace_back(point, *floor);
    }
    REQUIRE_FALSE(floors.empty());
    const auto unchangedCollision = [&] {
        fixture.scene->syncCollision(collision);
        CHECK(collision.triangleCount() == triangles);
        for (const auto& [point, before] : floors) {
            const auto after = collision.floorAt(point, .5f, .5f);
            REQUIRE(after);
            CHECK(after->y == before.y);
            CHECK(after->object == before.object);
            CHECK(after->objectFlags == before.objectFlags);
            CHECK(after->normal == before.normal);
            CHECK(after->point == before.point);
        }
    };
    const auto allocations = fixture.device.texturesCreated;
    fixture.scene->beginBossEntrance();
    for (usize index = 0; index < fixture.map.objects.size(); ++index) {
        if ((fixture.map.objects[index].flags & 0x200U) != 0) {
            CHECK(fixture.scene->worldObjectVisible(index));
        }
    }
    fixture.scene->capturePresentation();
    fixture.scene->update(.2f);
    usize moved = 0;
    std::vector<Mat4> poses;
    for (usize index = 0; index < fixture.map.objects.size(); ++index) {
        const auto& object = fixture.map.objects[index];
        poses.push_back(fixture.scene->worldObjectTransform(index));
        if ((object.flags & 0x200U) != 0) {
            if (poses.back() != Mat4{1}) {
                ++moved;
            }
        } else if (object.name.starts_with("FLOOR__FL")) {
            CHECK(fixture.scene->worldObjectVisible(index));
            CHECK(poses.back() == Mat4{1});
        }
    }
    CHECK(moved == 43);
    unchangedCollision();
    fixture.scene->beginBossEntrance(); // Repeated wake callbacks do not restart the eruption.
    for (usize index = 0; index < poses.size(); ++index) {
        CHECK(fixture.scene->worldObjectTransform(index) == poses[index]);
    }
    fixture.scene->update(3);
    unchangedCollision();
    for (usize index = 0; index < fixture.map.objects.size(); ++index) {
        const auto& object = fixture.map.objects[index];
        if ((object.flags & 0x200U) != 0) {
            CHECK_FALSE(fixture.scene->worldObjectVisible(index));
        } else if (object.name.starts_with("FLOOR__FL")) {
            CHECK(fixture.scene->worldObjectVisible(index));
        }
    }
    CHECK(fixture.device.texturesCreated == allocations);
}

TEST_CASE("Erupting native fragments draw, pick and highlight their same interpolated pose",
          "[legends][arena][boss-eruption][selection][assets]") {
    ArenaFixture fixture{"boss2"};
    usize objectIndex = 0;
    std::optional<MeshPart> triangle;
    bool cullBack = false;
    for (; objectIndex < fixture.map.objects.size() && !triangle; ++objectIndex) {
        const auto& object = fixture.map.objects[objectIndex];
        if ((object.flags & 0x200U) == 0) {
            continue;
        }
        REQUIRE(object.partCullBack.size() == object.mesh.parts.size());
        for (usize partIndex = 0; partIndex < object.mesh.parts.size(); ++partIndex) {
            const auto& part = object.mesh.parts[partIndex];
            for (usize at = 0; at + 2 < part.indices.size() && !triangle; at += 3) {
                const auto a = object.mesh.vertices[part.indices[at]].position;
                const auto b = object.mesh.vertices[part.indices[at + 1]].position;
                const auto c = object.mesh.vertices[part.indices[at + 2]].position;
                if (glm::length(glm::cross(b - a, c - a)) > .01f) {
                    triangle = MeshPart{
                        .texture = part.texture,
                        .indices = {part.indices[at], part.indices[at + 1], part.indices[at + 2]}};
                    cullBack = object.partCullBack[partIndex];
                }
            }
        }
    }
    REQUIRE(triangle);
    --objectIndex;
    std::array<Vec3, 3> authored{};
    for (usize i = 0; i < authored.size(); ++i) {
        const auto& object = fixture.map.objects[objectIndex];
        authored[i] =
            Vec3{object.transform * Vec4{object.mesh.vertices[triangle->indices[i]].position, 1}};
    }
    // Isolate one actual face but retain its native ID/anchor, culling and real
    // scripted placement. The scene keeps all fixtures, testing the real shared renderer.
    for (usize i = 0; i < fixture.map.objects.size(); ++i) {
        fixture.map.objects[i].mesh.parts =
            i == objectIndex ? std::vector{*triangle} : std::vector<MeshPart>{};
        fixture.map.objects[i].partCullBack =
            i == objectIndex ? std::vector<bool>{cullBack} : std::vector<bool>{};
    }
    auto* motion = fixture.scene.get();
    LegendsPreview preview;
    preview.open(fixture.device, fixture.map, std::move(fixture.scene));
    motion->beginBossEntrance();
    motion->update(.2f);
    motion->capturePresentation();
    motion->update(.1f);
    const WorldCamera view{};
    for (const f32 alpha : {0.0f, .5f, 1.0f}) {
        CAPTURE(alpha);
        fixture.device.draws.clear();
        preview.drawWorld(fixture.device, Mat4{1}, view, TreeModel::Pass::Opaque, alpha);
        preview.drawWorld(fixture.device, Mat4{1}, view, TreeModel::Pass::Blended, alpha);
        const auto drawn = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
            return draw.vertices.size() == 3 && draw.vertices.front().position == authored[0];
        });
        REQUIRE(drawn != fixture.device.draws.end());
        CHECK(drawn->transform == motion->worldObjectTransform(objectIndex, alpha));
        std::array<Vec3, 3> current{};
        for (usize i = 0; i < current.size(); ++i) {
            current[i] = Vec3{drawn->transform * Vec4{authored[i], 1}};
        }
        const Vec3 normal =
            glm::normalize(glm::cross(current[1] - current[0], current[2] - current[0]));
        const Vec3 centre = (current[0] + current[1] + current[2]) / 3.0f;
        const Vec3 eye = centre + normal * .01f;
        const auto hit = preview.pickObject(eye, -normal, view, alpha);
        REQUIRE(hit);
        CHECK(hit->object.id == static_cast<s32>(objectIndex));
        CHECK(hit->distance == Approx(.01f).margin(.001f));
        fixture.device.draws.clear();
        preview.highlightObject(fixture.device, Mat4{1}, view, alpha, static_cast<s32>(objectIndex),
                                SelectionHighlight{eye});
        REQUIRE(fixture.device.draws.size() == 2);
        const auto& wash = fixture.device.draws.back();
        REQUIRE(wash.vertices.size() == 3);
        for (usize i = 0; i < current.size(); ++i) {
            CHECK(glm::distance(wash.vertices[i].position,
                                current[i] + (eye - current[i]) * SelectionHighlight::kLift) <
                  .001f);
        }
    }
}

TEST_CASE("Native boss cover uses authored coordinates, tiers and damageable projectile bodies",
          "[legends][arena][assets]") {
    std::string_view stage = "mount6";
    usize count = 6;
    s32 tierHealth = 40;
    SECTION("Dragon cover") {}
    SECTION("Plague cover") {
        stage = "town5";
        count = 3;
        tierHealth = 80;
    }
    ArenaFixture fixture{stage};
    auto& scene = *fixture.scene;
    const auto anchors = scene.arenaAnchors();
    const auto targets = scene.arenaTargets();
    REQUIRE(targets.size() == count);
    REQUIRE(anchors.size() == count);
    REQUIRE(scene.arenaObstacles().size() == count);
    auto stops = scene.arenaMissileStops();
    REQUIRE(stops.size() == count);
    for (usize index = 0; index < count; ++index) {
        const auto& native = fixture.encounter.combat.arenaRocks[index];
        CHECK(targets[index].index == index);
        CHECK(targets[index].active);
        CHECK(Vec3{targets[index].placement[3]} == native.position);
        CHECK(anchors[index] == targets[index].placement);
        CHECK(stops[index].rock == 16384 + static_cast<s32>(index));
        CHECK(stops[index].rockHealth == tierHealth * 3);
        CHECK(stops[index].rockArmor == 0);
        CHECK(stops[index].box.centre == native.position);
        CHECK(stops[index].box.cylinderRadius == Approx(2.3f));
        CHECK(stops[index].box.height == Approx(5));
    }

    const Vec3 centre = fixture.encounter.combat.arenaRocks[0].position;
    const Vec3 from = centre + Vec3{-4, 2.5f, 0};
    const Vec3 to = centre + Vec3{4, 2.5f, 0};
    REQUIRE(scene.arenaBlocksSegment(from, to, .1f));
    const auto whole = fixture.geometryNear(centre);
    REQUIRE_FALSE(whole.empty());
    const auto allocations = fixture.device.texturesCreated;
    CHECK_FALSE(scene.strikeArenaRock(count, 1));
    CHECK_FALSE(scene.strikeArenaRock(0, -1));
    CHECK_FALSE(scene.strikeArenaRock(0, std::numeric_limits<f32>::infinity()));
    CHECK_FALSE(scene.strikeArenaRock(0, std::numeric_limits<f32>::quiet_NaN()));
    REQUIRE(scene.strikeArenaRock(0, static_cast<f32>(tierHealth)));
    CHECK(scene.arenaMissileStops()[0].rockHealth == tierHealth * 2);
    const auto damaged = fixture.geometryNear(centre);
    REQUIRE_FALSE(damaged.empty());
    CHECK(damaged != whole);
    REQUIRE(scene.strikeArenaRock(0, static_cast<f32>(tierHealth)));
    CHECK(scene.arenaMissileStops()[0].rockHealth == tierHealth);
    const auto low = fixture.geometryNear(centre);
    REQUIRE_FALSE(low.empty());
    CHECK(low != damaged);
    REQUIRE(scene.strikeArenaRock(0, static_cast<f32>(tierHealth)));
    CHECK_FALSE(scene.strikeArenaRock(0, 1));
    CHECK_FALSE(scene.arenaTargets()[0].active);
    CHECK_FALSE(scene.arenaBlocksSegment(from, to, .1f));
    CHECK(scene.arenaObstacles().size() == count - 1);
    stops = scene.arenaMissileStops();
    REQUIRE(stops.size() == count - 1);
    CHECK(stops.front().rock == 16385);     // Stable roster IDs do not compact after destruction.
    CHECK(scene.arenaAnchors() == anchors); // Boss effects retain destroyed cover anchors.
    for (usize tick = 0; tick < 180; ++tick) {
        scene.update(1.0f / 60);
    }
    CHECK_FALSE(scene.arenaTargets()[0].active);
    CHECK(fixture.device.texturesCreated == allocations);
}

TEST_CASE("Imported Yeti eruptions activate native anchors without borrowing GDL stage coordinates",
          "[legends][arena][assets]") {
    ArenaFixture fixture{"ice5"};
    auto& scene = *fixture.scene;
    const auto anchors = scene.arenaAnchors();
    REQUIRE(anchors.size() == 8);
    REQUIRE(scene.arenaTargets().size() == 8);
    REQUIRE(scene.arenaObstacles().empty());
    REQUIRE(scene.arenaMissileStops().empty());
    CHECK(std::ranges::none_of(scene.arenaTargets(), &CombatArenaTarget::active));
    CHECK_FALSE(scene.strikeArenaRock(3, 1));
    const auto allocations = fixture.device.texturesCreated;
    scene.scheduleArenaActivation(3, .5f);
    scene.update(.25f);
    CHECK_FALSE(scene.arenaTargets()[3].active);
    scene.update(.25f);
    REQUIRE(scene.arenaTargets()[3].active);
    auto stops = scene.arenaMissileStops();
    REQUIRE(stops.size() == 1);
    CHECK(stops[0].rock == 16387);
    CHECK(stops[0].rockHealth == 120);
    CHECK(stops[0].box.centre == fixture.encounter.combat.arenaRocks[3].position);
    REQUIRE_FALSE(fixture.geometryNear(stops[0].box.centre).empty());
    REQUIRE(scene.strikeArenaRock(3, 120));
    CHECK_FALSE(scene.arenaTargets()[3].active);
    scene.scheduleArenaActivation(3, .5f);
    scene.update(.5f);
    REQUIRE(scene.arenaTargets()[3].active);
    REQUIRE(scene.arenaMissileStops().size() == 1);
    CHECK(scene.arenaMissileStops()[0].rockHealth == 120);
    CHECK(scene.arenaAnchors() == anchors);
    CHECK(fixture.device.texturesCreated == allocations);
}
