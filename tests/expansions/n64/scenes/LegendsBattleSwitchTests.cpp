#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/shared/scenes/LegendsPreview.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct BattleBridge {
    test::FakeRenderDevice device;
    formats::LegendsMap map;
    LegendsPreview preview;
    WorldCollision collision;
    std::array<Vec3, 2> pads{};
    std::array<u32, 2> sounds{};
    usize bridge = 0;
    Vec3 floorPoint{0};
    Vec3 faceVertex{0};

    explicit BattleBridge(s32 playerCount = 2) {
        const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
        const auto directory = root / "levels/battle3";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.wad");
        map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects,
                                            readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        const auto definitions = formats::decodeLegendsN64Switches(
            readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
        CHECK(definitions.warnings.empty());
        const LegendsWorldMotion motion{map, definitions.switches};
        for (usize i = 0; i < pads.size(); ++i) {
            const auto nativeIndex = static_cast<u32>(43 + i);
            const auto plate = std::ranges::find(definitions.switches, nativeIndex,
                                                 &formats::LegendsN64Switch::nativeIndex);
            REQUIRE(plate != definitions.switches.end());
            CHECK(plate->templateKey == 0x1201);
            CHECK(plate->target == "DRAWB1");
            CHECK(plate->mode() == 0x100);
            CHECK(plate->supported);
            CHECK(plate->minimumPlayers == 0);
            REQUIRE(motion.platePosition(nativeIndex));
            pads[i] = *motion.platePosition(nativeIndex);
            sounds[i] = plate->sound;
            REQUIRE(sounds[i] != 0);
        }
        CHECK(pads[0].x == 10.25f);
        CHECK(pads[0].z == 84.25f);
        CHECK(pads[1].x == 22.25f);
        CHECK(pads[1].z == 101.75f);
        const auto object = std::ranges::find(map.objects, std::string{"DRAWB1_890_537"},
                                              &formats::LegendsMapObject::name);
        REQUIRE(object != map.objects.end());
        bridge = static_cast<usize>(object - map.objects.begin());
        const auto floor = std::ranges::find_if(map.collision, [&](const auto& face) {
            return face.owner == static_cast<s32>(bridge) && face.floor && face.normal.y > .5f;
        });
        REQUIRE(floor != map.collision.end());
        floorPoint = (floor->vertices[0] + floor->vertices[1] + floor->vertices[2]) / 3.0f;
        std::vector<CollisionTriangle> triangles;
        for (const auto& face : map.collision) {
            triangles.push_back({face.normal, face.vertices, face.owner,
                                 (face.floor ? 4U : 0U) | (face.wall ? 2U : 0U)});
        }
        collision.build(std::move(triangles));
        auto adapter =
            LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, playerCount);
        adapter->bindCollision(collision);
        for (const auto& warning : adapter->warnings()) {
            CHECK(warning.find("Native switch") == std::string::npos);
        }
        // Isolate one authored bridge triangle in the world draw, retaining
        // the full native collision and actual fixture bindings around it.
        REQUIRE_FALSE(object->mesh.parts.empty());
        const auto part = object->mesh.parts.front();
        REQUIRE(part.indices.size() >= 3);
        REQUIRE(object->partCullBack.size() == object->mesh.parts.size());
        const bool cullBack = object->partCullBack.front();
        const MeshPart face{.texture = part.texture,
                            .indices = {part.indices[0], part.indices[1], part.indices[2]}};
        faceVertex =
            Vec3{object->transform * Vec4{object->mesh.vertices[face.indices[0]].position, 1}};
        for (auto& value : map.objects) {
            value.mesh.parts.clear();
            value.partCullBack.clear();
        }
        object->mesh.parts.push_back(face);
        object->partCullBack.push_back(cullBack);
        preview.open(device, map, std::move(adapter));
    }

    LegendsSceneAdapter& adapter() { return *preview.adapter(); }
    void step(std::span<const LegendsPortalVisitor> visitors, f32 seconds) {
        adapter().touchSwitches(visitors);
        adapter().capturePresentation();
        adapter().update(seconds);
        adapter().syncCollision(collision);
    }
    bool hasFloor() const {
        const auto floor = collision.floorAt(floorPoint, .03f, .03f);
        return floor && floor->object == static_cast<s32>(bridge);
    }
    std::optional<test::RecordedDraw> drawBridge(f32 alpha = -1) {
        device.draws.clear();
        preview.drawWorld(device, Mat4{1}, preview.camera(), TreeModel::Pass::All, alpha);
        const auto draw = std::ranges::find_if(device.draws, [&](const auto& value) {
            return value.vertices.size() == 3 && value.vertices.front().position == faceVertex;
        });
        return draw == device.draws.end() ? std::nullopt : std::optional{*draw};
    }
};
} // namespace

TEST_CASE("Both native Battle3 pressure pads reveal the same walkable bridge with their native cue",
          "[legends][assets][switches][native-pressure-bridge]") {
    for (const s32 playerCount : std::array<s32, 3>{1, 2, 4}) {
        for (const usize pressed : std::array<usize, 2>{0, 1}) {
            CAPTURE(playerCount, pressed);
            BattleBridge fixture{playerCount};
            REQUIRE(fixture.collision.objectTransform(static_cast<s32>(fixture.bridge)));
            CHECK(*fixture.collision.objectTransform(static_cast<s32>(fixture.bridge)) == Mat4{1});
            CHECK_FALSE(fixture.hasFloor());
            CHECK_FALSE(fixture.drawBridge());
            CHECK(fixture.adapter().worldObjectAlpha(fixture.bridge) == 0);
            const std::array visitors{LegendsPortalVisitor{0, fixture.pads[pressed], true, .5f, 3}};
            fixture.step(visitors, .1875f);
            CHECK(fixture.adapter().worldObjectAlpha(fixture.bridge) == Approx(.5f));
            CHECK_FALSE(fixture.hasFloor());
            CHECK(fixture.adapter().worldObjectOffset(fixture.bridge) == Vec3{0});
            CHECK(*fixture.collision.objectTransform(static_cast<s32>(fixture.bridge)) == Mat4{1});
            const auto half = fixture.drawBridge();
            REQUIRE(half);
            CHECK(half->state.blend == BlendMode::Alpha);
            CHECK(half->state.depthWrite);
            CHECK(half->vertices.front().color.a == 127);
            const auto events = fixture.adapter().takeSounds();
            CHECK(std::ranges::count(events, fixture.sounds[pressed], &LegendsSoundEvent::token) ==
                  1);
            CHECK(fixture.adapter().soundLoops().empty());
            fixture.step(visitors, 0);
            CHECK(fixture.adapter().worldObjectAlpha(fixture.bridge) == Approx(.5f));
            fixture.step(visitors, .1875f);
            CHECK(fixture.adapter().worldObjectAlpha(fixture.bridge) == 1);
            CHECK(fixture.hasFloor());
            REQUIRE(fixture.drawBridge());
            CHECK(fixture.drawBridge()->vertices.front().color.a == 255);
            CHECK(fixture.adapter().takeSounds().empty());
            CHECK(fixture.adapter().soundLoops().empty());

            // Actual collision support, not proximity to a pad, holds a crossing.
            const auto floor = fixture.collision.floorAt(fixture.floorPoint, .03f, .03f);
            REQUIRE(floor);
            const std::array crossing{
                LegendsPortalVisitor{0, fixture.floorPoint, true, .5f, 3, floor->object}};
            fixture.step(crossing, 1);
            fixture.step(crossing, 1);
            CHECK(fixture.hasFloor());
            CHECK(fixture.adapter().worldObjectAlpha(fixture.bridge) == 1);
            // The common enemy floor owner uses the same native support gate.
            fixture.adapter().supportSwitches(std::array{floor->object});
            fixture.step({}, 1);
            CHECK(fixture.hasFloor());
            fixture.adapter().supportSwitches({});
            fixture.step({}, .1875f);
            CHECK_FALSE(fixture.hasFloor());
            CHECK(fixture.adapter().worldObjectAlpha(fixture.bridge) == Approx(.5f));
            fixture.step(visitors, .1875f);
            CHECK(fixture.hasFloor());
            fixture.step({}, .01f); // Native release still issues its final active-state request.
            fixture.step({}, .375f);
            CHECK_FALSE(fixture.hasFloor());
            CHECK_FALSE(fixture.drawBridge());
            CHECK(fixture.adapter().soundLoops().empty());
        }
    }
}

TEST_CASE("Battle3 pressure bridges ignore dead visitors and combine simultaneous party pads",
          "[legends][assets][switches][native-pressure-bridge]") {
    BattleBridge fixture;
    const std::array dead{
        LegendsPortalVisitor{0, fixture.pads[0], false, .5f, 3, static_cast<s32>(fixture.bridge)}};
    fixture.step(dead, 1);
    CHECK_FALSE(fixture.hasFloor());
    CHECK(fixture.adapter().takeSounds().empty());
    const std::array both{LegendsPortalVisitor{0, fixture.pads[0], true, .5f, 3},
                          LegendsPortalVisitor{1, fixture.pads[1], true, .5f, 3}};
    fixture.step(both, .375f);
    CHECK(fixture.hasFloor());
    fixture.adapter().takeSounds();
    fixture.step(std::span{both}.subspan(1), .5f);
    fixture.step(std::span{both}.subspan(1), .5f);
    CHECK(fixture.hasFloor());
    CHECK(fixture.adapter().takeSounds().empty());
}
