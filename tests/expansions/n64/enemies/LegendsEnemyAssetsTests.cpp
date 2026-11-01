#include <array>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/enemies/LegendsEnemyAssets.h"
#include "formats/AnimationTree.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/Enemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("All native swarm banks animate and draw through the common enemy pool",
          "[legends][native-enemy-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native enemy banks are not installed");
    const auto program = readFile(root / "_program/game.bin");
    for (s32 kind : {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 30, 31}) {
        DYNAMIC_SECTION("native species " << kind) {
            test::FakeRenderDevice device;
            Enemies enemies;
            enemies.open(device, {}, nullptr, 6, {}, 1);
            std::vector<std::string> warnings;
            REQUIRE(enemies.setArchiveLoader([&](s32 requested) {
                return loadLegendsEnemyAssets(root, requested, program, warnings);
            }));
            REQUIRE(enemies.loadKind(kind));
            REQUIRE(enemies.archive(kind));
            REQUIRE_FALSE(enemies.setArchiveLoader({})); // cached resource identity is immutable
            const auto& trees = enemies.archive(kind)->trees;
            const auto prefix = std::string(enemyKind(kind).prefix);
            for (s32 tier = 1; tier <= 3; ++tier) {
                const auto id = enemies.spawn({.kind = kind,
                                               .tier = tier,
                                               .position = {static_cast<f32>(tier * 10), 0, 0},
                                               .placed = true},
                                              {}, {});
                REQUIRE(id);
                REQUIRE(enemies.animatorOf(*id));
                CHECK(enemies.animatorOf(*id)->bound());
                REQUIRE(enemies.appearanceTree({kind, tier, 0, false}));
            }
            for (const auto suffix : {"A", "B", "S"}) {
                if (const auto tree = trees.find(prefix + suffix)) {
                    const s32 strength = suffix[0] == 'A' ? 4 : suffix[0] == 'B' ? 5 : 6;
                    REQUIRE(trees.tree(*tree).findSequence("READY"));
                    const auto id =
                        enemies.spawn({.kind = kind,
                                       .tier = strength,
                                       .position = {static_cast<f32>(strength * 10), 0, 0},
                                       .placed = true},
                                      {}, {});
                    REQUIRE(id);
                    CHECK(enemies.appearanceTree({kind, strength == 6 ? 1 : 2, strength, false}) ==
                          &trees.tree(*tree));
                    CHECK(enemies.animatorOf(*id)->bound());
                }
            }
            enemies.update(1, 1.0f / 30, {});
            enemies.draw(device, Mat4{1}, WorldLighting{});
            REQUIRE_FALSE(device.draws.empty());
            usize vertices = 0;
            for (const auto& draw : device.draws)
                vertices += draw.vertices.size();
            if (kind == kItKind) {
                // IT is authored as one textured billboard quad per creature,
                // not a polygonal character mesh: all three bodies must draw.
                CHECK(vertices == 18);
            } else {
                CHECK(vertices > 30);
            }
            const auto oldHealth = enemies.healthOf(0);
            enemies.hurt(0, EnemyHit{1, 0, {0, 0, 1}, 0});
            enemies.hurt(0, EnemyHit{2, 0, {0, 0, 1}, 0});
            const auto events = enemies.takeFeedback();
            if (kind == kItKind || kind == kDeathKind) {
                CHECK(events.empty()); // Neither produces ordinary swarm hit feedback.
                // Active Death spends one endurance point per attempted hit,
                // independent of weapon strength; It remains invulnerable.
                CHECK(enemies.healthOf(0) == oldHealth - (kind == kDeathKind ? 2 : 0));
            } else {
                REQUIRE(events.size() == 2);
                CHECK(events[0].remainingHealth == Catch::Approx(oldHealth - 1));
                CHECK(events[1].remainingHealth == Catch::Approx(oldHealth - 3));
            }
            for (const auto& warning : warnings) {
                INFO(warning);
                CHECK_FALSE(warning.starts_with("Unbound"));
            }
        }
    }
}
TEST_CASE("A native enemy source never silently falls back to a GameCube body",
          "[legends][native-enemy-assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, {}, nullptr, 1, {}, 1);
    REQUIRE(enemies.setArchiveLoader([](s32) { return std::unique_ptr<ItemArchive>{}; }));
    CHECK_FALSE(enemies.loadKind(kItKind));
    CHECK_FALSE(enemies.spawn({.kind = kItKind, .placed = true}, {}, {}));
    enemies.open(device, {}, nullptr, 1, {}, 1);
    CHECK(enemies.loadKind(kItKind)); // the pre-existing GDL bodiless It path is unchanged
}

TEST_CASE("Native enemy groups retain debris and tags outside the action BODY",
          "[legends][native-enemy-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native enemy banks are not installed");
    const auto program = readFile(root / "_program/game.bin");
    const auto tree = [](std::string name, std::string action, usize nodes) {
        formats::TreeDefinition result;
        result.name = std::move(name);
        result.nodes.resize(nodes);
        result.sequences.push_back({.name = std::move(action), .frameCount = 1, .frameRate = 30});
        return result;
    };
    formats::AnimationFile animation;
    animation.trees = {tree("TROS:REDY", "REDY", 3), tree("TROS:RUN", "RUN", 3),
                       tree("TROS:EXP", "EXP", 1)};
    translateLegendsEnemyAnimations(animation, 1, program);
    REQUIRE(animation.trees.size() == 2);
    CHECK(animation.trees[0].name == "TROS");
    CHECK(animation.trees[0].nodes.size() == 3);
    REQUIRE(animation.trees[0].sequences.size() == 2);
    CHECK(animation.trees[0].sequences[0].name == "READY");
    CHECK(animation.trees[1].name == "SUICIDEEXP");
    CHECK(animation.trees[1].nodes.size() == 1);
    animation.trees = {tree("IT:REDY", "REDY", 1), tree("IT:WALK", "WALK", 1),
                       tree("IT:SIGN", "SIGN", 1)};
    animation.trees[2].nodes[0].position = {1, 2, 3};
    translateLegendsEnemyAnimations(animation, kItKind, program);
    REQUIRE(animation.trees.size() == 2);
    CHECK(animation.trees[0].name == "IT1");
    CHECK(animation.trees[1].name == "IT:SIGN");
    CHECK(animation.trees[1].nodes[0].position == Vec3{1, 2, 3});
    animation.trees = {tree("TROS:REDY", "REDY", 3), tree("TROS:RUN", "RUN", 1)};
    CHECK_THROWS(translateLegendsEnemyAnimations(animation, 1, program));
}
} // namespace

TEST_CASE("All native Ice tiers render a body above ground during emergence",
          "[legends][native-enemy-assets][ice-emergence][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "ice/seq.wad"))
        SKIP("N64 Ice enemy bank is not installed");
    std::vector<std::string> warnings;
    auto archive = loadLegendsEnemyAssets(root, 16, readFile(root / "_program/game.bin"), warnings);
    REQUIRE(archive);
    for (const auto name : {"ICE1", "ICE2", "ICE3"}) {
        CAPTURE(name);
        const auto index = archive->trees.find(name);
        REQUIRE(index);
        const auto& tree = archive->trees.tree(*index);
        const auto sequence = tree.findSequence("START");
        REQUIRE(sequence);
        REQUIRE(tree.sequences[*sequence].frames == 20);
        test::FakeRenderDevice device;
        TreeModel model;
        REQUIRE(model.bind(tree, archive->models, archive->textures, device));
        for (const s32 frame : {10, 19}) {
            CAPTURE(frame);
            TreePose pose;
            pose.evaluate(tree, *sequence, static_cast<f32>(frame));
            model.setFrame(*sequence, frame);
            device.draws.clear();
            model.draw(device, Mat4{1}, Mat4{1}, WorldLighting{}, pose.matrices());
            f32 low = 1000, high = -1000;
            usize bodyVertices = 0;
            bool visibleTexture = false;
            for (const auto& draw : device.draws) {
                if (draw.vertices.size() <= 6)
                    continue; // A headlamp quad alone is not a visible miner.
                for (const auto& vertex : draw.vertices) {
                    const Vec3 point{draw.transform * Vec4{vertex.position, 1}};
                    low = std::min(low, point.y);
                    high = std::max(high, point.y);
                    ++bodyVertices;
                }
                const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
                REQUIRE(texture);
                for (usize i = 3; i < texture->pixels.size(); i += 4)
                    visibleTexture |= texture->pixels[i] > 0;
            }
            CHECK(bodyVertices > 100);
            CHECK(high > 3);
            CHECK(high - low > 3);
            CHECK(visibleTexture);
        }
    }
}

TEST_CASE("Native Death loads the shared drain arc and animates it until contact ends",
          "[legends][native-enemy-assets][native-death-arc][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native enemy banks are not installed");
    std::vector<std::string> warnings;
    auto archive =
        loadLegendsEnemyAssets(root, kDeathKind, readFile(root / "_program/game.bin"), warnings);
    REQUIRE(archive);
    REQUIRE(archive->trees.find("DEATH_ARC"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting settings;
    settings.persistent = true;
    settings.depthWrite = false;
    const u32 id = effects.startSet(device, *archive, "DEATH_ARC", Vec3{0}, settings);
    REQUIRE(id != 0);
    std::set<const Texture*> textures;
    for (s32 tick = 0; tick < 60; ++tick) {
        effects.update(1.0f / 30);
        device.draws.clear();
        effects.draw(device, Mat4{1}, WorldLighting{});
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK_FALSE(draw.state.depthWrite);
            textures.insert(draw.texture);
        }
    }
    CHECK(textures.size() > 1);
    effects.stop(id);
    device.draws.clear();
    effects.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty());
}
