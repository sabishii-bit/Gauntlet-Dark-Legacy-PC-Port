#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/assets/LegendsN64Archive.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/combat/DamageTypes.h"

namespace {
using namespace gdl;
using namespace gdl::game;
std::vector<Vec3> nearby(LegendsSceneAdapter& scene, test::FakeRenderDevice& device,
                         const Vec3& centre, f32 radius = 6) {
    device.draws.clear();
    const auto camera = CameraFrame::of(WorldCamera{});
    scene.draw(device, Mat4{1}, camera, TreeModel::Pass::Opaque);
    scene.draw(device, Mat4{1}, camera, TreeModel::Pass::Blended);
    std::vector<Vec3> vertices;
    for (const auto& draw : device.draws) {
        for (const auto& vertex : draw.vertices) {
            const Vec3 point{draw.transform * Vec4{vertex.position, 1}};
            if (glm::distance(point, centre) < radius) {
                vertices.push_back(point);
            }
        }
    }
    return vertices;
}
} // namespace

TEST_CASE("Native Ice steam has visible animated frames from every camera direction",
          "[legends][native-traps][ice-steam-art][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/ice/objects.wad"))
        SKIP("N64 Ice fixture extraction is not installed");
    std::vector<std::string> warnings;
    auto archive =
        loadLegendsN64Archive(root / "items/ice", warnings, readFile(root / "_program/game.bin"));
    REQUIRE(archive);
    const auto tree = archive->trees.find("TRAP:TFVA");
    REQUIRE(tree);
    test::FakeRenderDevice device;
    ItemFigure figure;
    REQUIRE(figure.place(device, *archive, "TRAP:TFVA", ItemInstance{}, nullptr));
    figure.play(0, ItemFigure::Playback::Once);
    std::set<const Texture*> visibleFrames;
    for (s32 frame = 0; frame < 30; ++frame) {
        for (const f32 yaw : {0.0f, kPi * .5f, kPi, kPi * 1.5f}) {
            CAPTURE(frame, yaw);
            WorldCamera camera{{-35 * std::sin(yaw), 25, -35 * std::cos(yaw)}, .6f, yaw, 0};
            const auto view = CameraFrame::of(camera);
            device.draws.clear();
            figure.draw(device, Mat4{1}, WorldLighting{}, 1, 1, &view);
            bool plume = false;
            for (const auto& draw : device.draws) {
                Vec3 lo{1e6f}, hi{-1e6f};
                u8 alpha = 0;
                for (const auto& v : draw.vertices) {
                    const Vec3 p{draw.transform * Vec4{v.position, 1}};
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                    alpha = std::max(alpha, v.color.a);
                }
                if (hi.y - lo.y < 4)
                    continue; // The ground vent is a separate, always-visible flat quad.
                plume = true;
                CHECK(alpha > 0);
                CHECK(draw.state.depthTest);
                CHECK_FALSE(draw.state.depthWrite);
                CHECK(draw.state.blend == BlendMode::Alpha);
                u8 peak = 0;
                const auto& tex = dynamic_cast<const test::FakeTexture&>(*draw.texture);
                for (usize p = 3; p < tex.pixels.size(); p += 4)
                    peak = std::max(peak, tex.pixels[p]);
                if (frame > 0) {
                    CHECK(peak > 0);
                    visibleFrames.insert(draw.texture);
                }
                // The plume must face the camera rather than become edge-on.
                f32 acrossMin = 1e6f, acrossMax = -1e6f;
                for (const auto& v : draw.vertices) {
                    const f32 across =
                        glm::dot(Vec3{draw.transform * Vec4{v.position, 1}}, camera.right());
                    acrossMin = std::min(acrossMin, across);
                    acrossMax = std::max(acrossMax, across);
                }
                CHECK(acrossMax - acrossMin > 2);
            }
            CHECK(plume);
        }
        figure.update(1.0f / 30);
    }
    CHECK(visibleFrames.size() > 10);
}

TEST_CASE("Native Ice vents activate their steam artwork and damaging pulse",
          "[legends][native-traps][ice-vents][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "levels/ice1/worlds.wad"))
        SKIP("N64 Legends extraction is not installed");
    for (const auto level : {"ice1", "ice2", "ice3", "ice4"}) {
        DYNAMIC_SECTION(level) {
            const auto directory = root / "levels" / level;
            auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                     readFile(directory / "objects.wad"));
            const auto decoded = formats::decodeLegendsN64Traps(
                readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
            REQUIRE_FALSE(decoded.traps.empty());
            test::FakeRenderDevice device;
            auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
            REQUIRE(scene);
            std::vector<LegendsPortalVisitor> visitors;
            std::map<u32, usize> hits;
            for (const auto& trap : decoded.traps) {
                formats::LegendsN64Fixture placement;
                placement.kind = formats::LegendsN64FixtureKind::Trap;
                placement.position = trap.position;
                placement.groundProjected = trap.groundProjected;
                visitors.push_back(
                    {0, formats::legendsN64FixturePosition(placement, map.collision)});
                hits.emplace(trap.nativeIndex, 0);
            }
            const auto idle = nearby(*scene, device, visitors.front().position);
            std::set<const Texture*> textures;
            const auto rememberTextures = [&] {
                for (const auto& draw : device.draws) {
                    if (std::ranges::any_of(draw.vertices, [&](const auto& vertex) {
                            return glm::distance(Vec3{draw.transform * Vec4{vertex.position, 1}},
                                                 visitors.front().position) < 6;
                        }))
                        textures.insert(draw.texture);
                }
            };
            rememberTextures();
            const auto idleTextures = textures.size();
            bool animated = false;
            for (s32 tick = 0; tick < 240; ++tick) {
                scene->capturePresentation();
                scene->update(1.0f / 30);
                scene->updateTraps(1.0f / 30, false);
                scene->touchTraps(visitors);
                for (const auto& hit : scene->takeTrapHits()) {
                    ++hits.at(hit.nativeIndex);
                    CHECK(hit.damage > 0);
                }
                if (tick % 3 == 0) {
                    animated |= nearby(*scene, device, visitors.front().position) != idle;
                    rememberTextures();
                }
            }
            CHECK((animated || textures.size() > idleTextures));
            for (const auto& [index, count] : hits) {
                CAPTURE(index);
                CHECK(count > 0);
            }
        }
    }
}

TEST_CASE("Native Mount1 fire trap animates and emits one hit per player per pulse",
          "[legends][native-traps][scene-adapter][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/mount1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    const auto decoded = formats::decodeLegendsN64Traps(readFile(directory / "ipr.rom"),
                                                        readFile(root / "items/generic/ptrap.rom"));
    const auto found = std::ranges::find(decoded.traps, 9U, &formats::LegendsN64Trap::nativeIndex);
    REQUIRE(found != decoded.traps.end());
    REQUIRE(found->templateKey == 0x1003);
    formats::LegendsN64Fixture placement;
    placement.kind = formats::LegendsN64FixtureKind::Trap;
    placement.position = found->position;
    placement.groundProjected = found->groundProjected;
    const Vec3 position = formats::legendsN64FixturePosition(placement, map.collision);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    const auto idle = nearby(*scene, device, position);
    REQUIRE_FALSE(idle.empty());
    const std::array visitors{LegendsPortalVisitor{0, position}, LegendsPortalVisitor{1, position},
                              LegendsPortalVisitor{2, position + Vec3{0, 20, 0}}};
    WorldCamera camera;
    camera.position = position + Vec3{0, 1, 10}; // Native trap is behind this view.
    scene->setTrapView(ViewVolume::of(camera, 1.5f, 1));
    for (s32 i = 0; i < 240; ++i) {
        scene->capturePresentation();
        scene->update(1.0f / 60);
        scene->updateTraps(1.0f / 60, false);
        scene->touchTraps(visitors);
        for (const auto& hit : scene->takeTrapHits()) {
            CHECK(hit.nativeIndex != 9);
        }
    }
    camera.position = position + Vec3{0, 1, -10};
    scene->setTrapView(ViewVolume::of(camera, 1.5f, 1));
    std::map<u8, usize> hits;
    bool changed = false;
    for (s32 i = 0; i < 360; ++i) {
        scene->capturePresentation();
        scene->update(1.0f / 60);
        scene->updateTraps(1.0f / 60, false);
        scene->touchTraps(visitors);
        for (const auto& hit : scene->takeTrapHits()) {
            if (hit.nativeIndex == 9) {
                ++hits[hit.player];
                CHECK(hit.damage == 4);
                CHECK_FALSE(hit.pierces);
            }
        }
        if (i % 10 == 0) {
            changed |= nearby(*scene, device, position) != idle;
        }
    }
    CHECK(changed);
    CHECK(hits[0] == 1);
    CHECK(hits[1] == 1);
    CHECK(hits[2] == 0);
    CHECK(scene->takeTrapHits().empty());
}

TEST_CASE("All native Castle tentacles bind animated HITNODE contact without root substitutes",
          "[legends][native-traps][scene-adapter][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto archive = root / "items/castle/tent";
    if (!std::filesystem::is_regular_file(archive / "objects.wad")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto program = readFile(root / "_program/game.bin");
    const auto models = formats::decodeLegendsN64Models(readFile(archive / "objects.wad"));
    const auto dynamic = std::filesystem::is_regular_file(archive / "dynseq.rom")
                             ? readFile(archive / "dynseq.rom")
                             : std::vector<u8>{};
    auto decoded =
        formats::decodeLegendsN64Sequences(readFile(archive / "seq.wad"), models, program, dynamic);
    const auto animations = AnimationSet::fromDecoded(std::move(decoded.animation));
    usize total = 0;
    for (const auto* level : {"castle1", "castle6"}) {
        {
            CAPTURE(level);
            const auto directory = root / "levels" / level;
            auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                     readFile(directory / "objects.wad"));
            const auto definitions = formats::decodeLegendsN64Traps(
                readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
            std::map<u32, u8> expected;
            std::vector<LegendsPortalVisitor> visitors;
            for (const auto& trap : definitions.traps) {
                if (trap.contactNode.empty()) {
                    continue;
                }
                ++total;
                expected.emplace(trap.nativeIndex, trap.damage);
                const auto index =
                    animations.find(formats::legendsN64TreeName("TRAP", trap.activeSequence));
                REQUIRE(index);
                const auto& tree = animations.tree(*index);
                const auto node =
                    std::ranges::find(tree.nodes, trap.contactNode, &TreeNodeInfo::name);
                REQUIRE(node != tree.nodes.end());
                const usize nodeIndex = static_cast<usize>(node - tree.nodes.begin());
                REQUIRE_FALSE(tree.sequences.empty());
                REQUIRE(tree.sequences.front().frames > 30);
                formats::LegendsN64Fixture placement;
                placement.kind = formats::LegendsN64FixtureKind::Trap;
                placement.position = trap.position;
                placement.groundProjected = trap.groundProjected;
                const auto world = glm::scale(
                    itemPlacement(formats::legendsN64FixturePosition(placement, map.collision),
                                  Vec3{0, trap.yaw, 0}),
                    Vec3{trap.scale});
                TreePose pose;
                std::optional<Vec3> first;
                bool moved = false;
                for (s32 frame = 0; frame < tree.sequences.front().frames; ++frame) {
                    pose.evaluate(tree, 0, static_cast<f32>(frame));
                    const Vec3 point{(world * pose.matrices()[nodeIndex])[3]};
                    if (!first) {
                        first = point;
                    }
                    moved |= glm::distance(point, *first) > 1;
                    // Occupy the actual authored swept path, not a sphere around the root.
                    visitors.push_back({0, point - Vec3{0, 1.5f, 0}, true, .5f, 3});
                }
                CHECK(moved);
            }
            test::FakeRenderDevice device;
            auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
            for (const auto& warning : scene->warnings()) {
                INFO(warning);
                CHECK(warning.find("trap attachment") == std::string::npos);
                CHECK(warning.find("trap active artwork") == std::string::npos);
            }
            std::map<u32, usize> reached;
            bool testedStopTime = false;
            for (s32 tick = 0; tick < 300; ++tick) {
                scene->capturePresentation();
                scene->update(1.0f / 60);
                scene->updateTraps(1.0f / 60, false);
                scene->touchTraps(visitors);
                for (const auto& hit : scene->takeTrapHits()) {
                    const auto found = expected.find(hit.nativeIndex);
                    if (found != expected.end()) {
                        ++reached[hit.nativeIndex];
                        CHECK(hit.damage == found->second);
                        CHECK((hit.impact.flags & damage::kKnockDown) != 0);
                    }
                }
                if (!testedStopTime && !reached.empty()) {
                    const auto before = nearby(*scene, device, Vec3{0}, 2000);
                    bool animated = false;
                    for (s32 stoppedTick = 0; stoppedTick < 60; ++stoppedTick) {
                        scene->capturePresentation();
                        scene->updateTraps(1.0f / 60, true);
                        scene->touchTraps(visitors);
                        CHECK(scene->takeTrapHits().empty());
                        animated |= nearby(*scene, device, Vec3{0}, 2000) != before;
                    }
                    CHECK(animated);
                    testedStopTime = true;
                }
            }
            CHECK(testedStopTime);
            for (const auto& [index, damage] : expected) {
                CAPTURE(level, index, damage);
                CHECK(reached[index] == 1);
            }
        }
    }
    CHECK(total == 9);
}

TEST_CASE("Native damaging traps bind artwork and attachments across admitted party rosters",
          "[legends][native-traps][scene-adapter-corpus][assets]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    const auto root = assets / "n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/ptrap.rom")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto catalog = legendsMaps(LegendsPlatform::N64, assets, {}, {});
    REQUIRE(catalog.size() == 34);
    const auto templates = readFile(root / "items/generic/ptrap.rom");
    usize decodedCount = 0;
    usize loadedRosters = 0;
    for (const auto& entry : catalog) {
        CAPTURE(entry.name);
        const auto decoded =
            formats::decodeLegendsN64Traps(readFile(entry.directory / "ipr.rom"), templates);
        REQUIRE(decoded.warnings.empty());
        decodedCount += decoded.traps.size();
        if (decoded.traps.empty()) {
            continue;
        }
        std::vector<std::vector<u32>> checkedRosters;
        for (s32 players = 1; players <= 4; ++players) {
            CAPTURE(players);
            std::vector<u32> admitted;
            for (const auto& trap : decoded.traps) {
                if (shownToParty(trap.minimumPlayers, players)) {
                    admitted.push_back(trap.nativeIndex);
                }
            }
            if (admitted.empty() ||
                std::ranges::find(checkedRosters, admitted) != checkedRosters.end()) {
                continue;
            }
            checkedRosters.push_back(admitted);
            ++loadedRosters;
            auto map = formats::LegendsMap::parseN64(readFile(entry.directory / "worlds.wad"),
                                                     readFile(entry.directory / "objects.wad"));
            test::FakeRenderDevice device;
            auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, entry.directory,
                                                   map, players);
            REQUIRE(scene != nullptr);
            for (const auto& warning : scene->warnings()) {
                INFO(warning);
                // Unlike a successful IPR decode, these cover the real archive scope,
                // active/S/E variant binding, authored durations and HITNODE lookup.
                CHECK_FALSE(warning.starts_with("Native damaging trap"));
                CHECK_FALSE(warning.starts_with("Native trap stage damage scale"));
                for (const auto& trap : decoded.traps) {
                    if (shownToParty(trap.minimumPlayers, players)) {
                        CAPTURE(trap.nativeIndex, trap.templateKey, trap.minimumPlayers);
                        CHECK(warning != "Unbound fixture: " + formats::legendsN64TreeName(
                                                                   "TRAP", trap.idleSequence));
                    }
                }
            }
            // Exercise the selected runtime lane as well as construction; visibility
            // is deliberately unspecified so native activation is allowed headlessly.
            scene->capturePresentation();
            scene->update(1.0f / 30);
            scene->updateTraps(1.0f / 30, false);
        }
        CHECK_FALSE(checkedRosters.empty());
    }
    CHECK(decodedCount == 538);
    CHECK(loadedRosters > 0);
}
