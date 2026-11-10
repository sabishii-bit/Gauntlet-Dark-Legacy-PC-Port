#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/level/LegendsDreamcastObelisks.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "levels/levelb1/worlds.rom")) {
        SKIP("Native Dreamcast Mountain Valley is not installed");
    }
    return root;
}

TEST_CASE("Dreamcast obelisk uses its authored sprite and colour transition",
          "[legends][dreamcast-obelisks][assets]") {
    const auto root = nativeRoot();
    auto archive = loadLegendsDreamcastArchive(root / "items/levelb");
    REQUIRE(archive);
    const auto index = archive->trees.find("OBELISK");
    REQUIRE(index);
    const auto& tree = archive->trees.tree(*index);
    REQUIRE(tree.findSequence("OFF"));
    REQUIRE(tree.findSequence("ONA"));
    REQUIRE(tree.findSequence("ON"));
    REQUIRE(tree.findNode("OBLISK"));
    REQUIRE(tree.findNode("OBLISKY"));
    CHECK(tree.sequences[*tree.findSequence("ONA")].frames == 50);
    CHECK(tree.sequences[*tree.findSequence("ONA")].frameRate == 30);
    CHECK(std::ranges::count_if(
              tree.nodes, [](const auto& node) { return node.name.starts_with("XPFF#"); }) == 12);
    test::FakeRenderDevice device;
    ItemFigure figure;
    REQUIRE(figure.place(device, *archive, "OBELISK", {}, nullptr));
    const auto inspect = [&](s32 sequence, f32 seconds) {
        figure.play(sequence, ItemFigure::Playback::Once);
        figure.update(seconds);
        device.draws.clear();
        const auto camera = CameraFrame::of(WorldCamera{});
        figure.draw(device, Mat4{1}, WorldLighting{}, 1, 1, &camera);
        return device.draws;
    };
    const auto scaleOf = [&](std::string_view node) {
        const auto transform = figure.nodeTransform(node);
        REQUIRE(transform);
        return glm::length(Vec3{(*transform)[0]});
    };
    const auto spriteArea = [](const std::vector<test::RecordedDraw>& draws) {
        f64 area = 0;
        for (const auto& draw : draws) {
            if (draw.state.blend != BlendMode::Additive) {
                continue;
            }
            for (usize vertex = 0; vertex + 2 < draw.vertices.size(); vertex += 3) {
                const auto& a = draw.vertices[vertex].position;
                const auto& b = draw.vertices[vertex + 1].position;
                const auto& c = draw.vertices[vertex + 2].position;
                area += .5 * glm::length(glm::cross(b - a, c - a));
            }
        }
        return area;
    };
    const auto off = inspect(static_cast<s32>(*tree.findSequence("OFF")), 0);
    REQUIRE_FALSE(off.empty());
    CHECK(scaleOf("OBLISK") == 1);
    CHECK(scaleOf("OBLISKY") < .002f);
    CHECK(scaleOf("XPFF#01") < .002f);
    const auto active = inspect(static_cast<s32>(*tree.findSequence("ONA")), .5f);
    // OFF retains the same draws at 0.001 scale. Count submitted triangle area,
    // not draw calls, to prove the authored rising sprites become visible.
    CHECK(spriteArea(active) > spriteArea(off) * 10000);
    CHECK(scaleOf("XPFF#01") > .9f);
    CHECK(scaleOf("OBLISK") < .01f);
    CHECK(scaleOf("OBLISKY") > .99f);
    const auto firstGlow = figure.nodeTransform("SMALLGLOW");
    REQUIRE(firstGlow);
    CHECK(std::ranges::any_of(active, [](const auto& draw) {
        return draw.state.blend == BlendMode::Additive && !draw.state.depthWrite;
    }));
    inspect(static_cast<s32>(*tree.findSequence("ONA")), 1);
    const auto laterGlow = figure.nodeTransform("SMALLGLOW");
    REQUIRE(laterGlow);
    CHECK((*laterGlow)[3].y > (*firstGlow)[3].y + 2);
    const auto on = inspect(static_cast<s32>(*tree.findSequence("ON")), 0);
    REQUIRE_FALSE(on.empty());
    CHECK(scaleOf("OBLISK") < .002f);
    CHECK(scaleOf("OBLISKY") == 1);
    CHECK(spriteArea(on) < spriteArea(active) * .01);

    // The colour change is the native OBLISK -> OBLISKY mesh, not a tint.
    // Verify its authored texture differs and is actually submitted in both
    // ONA and ON; inactive shrunken meshes may precede it in the draw list.
    const auto plain = archive->models.find(tree.nodes[*tree.findNode("OBLISK")].object);
    const auto lit = archive->models.find(tree.nodes[*tree.findNode("OBLISKY")].object);
    REQUIRE(plain);
    REQUIRE(lit);
    const auto& plainMesh = archive->models.mesh(*plain);
    const auto& litMesh = archive->models.mesh(*lit);
    REQUIRE_FALSE(plainMesh.parts.empty());
    REQUIRE_FALSE(litMesh.parts.empty());
    const auto litTexture = litMesh.parts.front().texture;
    CHECK(litTexture != plainMesh.parts.front().texture);
    const auto* texture = archive->textures.cachedTexture(litTexture);
    REQUIRE(texture);
    CHECK(std::ranges::any_of(active, [&](const auto& draw) { return draw.texture == texture; }));
    CHECK(std::ranges::any_of(on, [&](const auto& draw) { return draw.texture == texture; }));
}

TEST_CASE("Dreamcast Mountain Valley obelisk grants once and remains solid when restored",
          "[legends][dreamcast-obelisks][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levelb1";
    const auto models = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), models);
    map.textures =
        formats::decodeLegendsDreamcastTextures(models, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 2);
    CharacterSave save;
    CharacterSave partner;
    save.character = 0;
    partner.character = 1;
    const std::array<const CharacterSave*, 2> progress{&save, &partner};
    scene->synchronizeProgress(progress);
    // A copy, not a reference: the scene is reloaded below and frees the pickups it held.
    const auto find = [&]() -> LegendsPickup {
        const auto pickups = scene->pickups();
        const auto at = std::ranges::find_if(pickups, [](const auto& pickup) {
            return pickup.award && pickup.award->kind == LegendsRelic::Obelisk;
        });
        REQUIRE(at != pickups.end());
        return *at;
    };
    const auto pickup = find();
    REQUIRE_FALSE(pickup.collected);
    const auto checkBody = [&] {
        const auto bodies = scene->containerObstacles();
        const auto at = std::ranges::find(bodies, pickup.position, &Obstacle::centre);
        REQUIRE(at != bodies.end());
        CHECK(at->solid);
        CHECK(at->pushOut(pickup.position, .5f) != pickup.position);
    };
    checkBody();

    SECTION("Gameplay receipt does not add a GameCube pickup sound") {
        PlayerActor actor;
        actor.spawn(0, save, nullptr, pickup.position, 0);
        LegendsLevel level{device, map, std::move(scene), directory, test::dataDirectory()};
        std::array<PlayerActor*, 1> players{&actor};
        level.updateParty(players, 1.0f / 60, false);
        const auto receipts = level.takePickups();
        const auto receipt = std::ranges::find(receipts, pickup.name, &ImportedPickup::name);
        REQUIRE(receipt != receipts.end());
        CHECK(receipt->taking.sound.empty());
        CHECK(receipt->taking.took());
        return;
    }
    SECTION("Animation, party award and persistent body") {
        PlayerActor actor;
        actor.spawn(0, save, nullptr, pickup.position, 0);
        const std::array<CharacterSave*, 1> recipients{&partner};
        REQUIRE_FALSE(scene->collect(actor, 1, false, {}, recipients).empty());
        CHECK(find().collected);
        for (const auto* member : std::array{&actor.save(), &partner}) {
            CHECK(member->progress()
                      .legendsProgress(LegendsPlatform::Dreamcast)
                      .has(LegendsRelic::Obelisk, pickup.award->index));
        }
        const auto sounds = scene->takeSounds();
        CHECK(std::ranges::count(sounds, kDreamcastObeliskSound, &LegendsSoundEvent::token) == 1);
        CHECK(
            scene->takeObeliskSpeech().empty()); // Never feed DC identities to the N64 voice table.
        const auto created = device.texturesCreated;
        for (s32 tick = 0; tick < 150; ++tick) {
            scene->capturePresentation();
            scene->update(1.0f / 60);
            scene->collect(actor, 1, false, {}, recipients);
        }
        CHECK(device.texturesCreated == created);
        const auto repeated = scene->takeSounds();
        CHECK(std::ranges::count(repeated, kDreamcastObeliskSound, &LegendsSoundEvent::token) == 0);
        checkBody();
        scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 2);
        scene->synchronizeProgress(std::array<const CharacterSave*, 2>{&actor.save(), &partner});
        CHECK(find().collected);
        CHECK(scene->takeSounds().empty());
        checkBody();
    }
}
} // namespace
