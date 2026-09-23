#include <algorithm>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/LegendItems.h"
#include "game/world/EffectTrees.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("effect transforms retain pitch roll and scale from a full attachment",
          "[game][world][effects]") {
    EffectTrees::Effect effect;
    effect.scale = 2;
    effect.position = {5, 6, 7};
    REQUIRE(effect.transform() == glm::scale(glm::translate(Mat4{1}, effect.position), Vec3{2}));
    effect.attachment = glm::rotate(Mat4{1}, 0.7f, glm::normalize(Vec3{1, 0, 1}));
    Mat4 expected = glm::scale(*effect.attachment, Vec3{2});
    expected[3] = Vec4{effect.position, 1};
    REQUIRE(effect.transform() == expected);
    effect.yaw = 1; // full attachment replaces the world-yaw path
    REQUIRE(effect.transform() == expected);
}

TEST_CASE("Wraith's waiting portal retains its authored static scale throughout its hold",
          "[game][world][effects][wraith][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/WRAITH/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.seconds = 100;
    REQUIRE(effects.startSet(device, archive, "INITFX", Vec3{0}, setting) != 0);
    for (const f32 elapsed : {0.0f, 0.5f, 2.0f}) {
        effects.update(elapsed);
        REQUIRE(effects.count() == 1);
        const auto& effect = effects.effect(0);
        REQUIRE(effect.pose.poses().size() == 5);
        const Vec3 scale = effect.pose.poses()[1].scale;
        REQUIRE(scale.x == Catch::Approx(0.001f));
        REQUIRE(scale.y == Catch::Approx(0.001f));
        REQUIRE(scale.z == Catch::Approx(0.001f));
        REQUIRE(glm::length(Vec3{effect.pose.matrices()[2][0]}) == Catch::Approx(0.001f));
        device.draws.clear();
        effects.draw(device, Mat4{1}, {});
        REQUIRE_FALSE(device.draws.empty());
    }
}

TEST_CASE("Wraith emergence smoke preserves its growing geometry when facing the camera",
          "[game][world][effects][wraith][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/WRAITH/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    REQUIRE(effects.start(device, archive, "GENFX2", Vec3{0}));
    effects.update(2.0f);
    REQUIRE(effects.effect(0).pose.poses()[0].scale.y > 1.0f);
    effects.draw(device, Mat4{1}, {});
    const auto ordinary = device.draws;
    REQUIRE_FALSE(ordinary.empty());
    device.draws.clear();
    const CameraFrame camera = CameraFrame::at({0, 10, -20});
    effects.draw(device, Mat4{1}, {}, &camera);
    REQUIRE(device.draws.size() == ordinary.size());
    for (usize i = 0; i < ordinary.size(); ++i) {
        const auto& before = ordinary[i].vertices;
        const auto& after = device.draws[i].vertices;
        REQUIRE(before.size() >= 3);
        REQUIRE(after.size() == before.size());
        const f32 original = glm::length(before[1].position - before[0].position);
        REQUIRE(original > 0);
        REQUIRE(glm::length(after[1].position - after[0].position) == Catch::Approx(original));
    }
}

TEST_CASE("Yeti stomp geometry draws through the floor using its authored depth policy",
          "[game][world][effects][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/YETI/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    REQUIRE(effects.start(device, archive, "ATTACK4FX", Vec3{0, -3, 0}));
    effects.update(0.2f);
    effects.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        REQUIRE_FALSE(draw.state.depthTest);
        REQUIRE_FALSE(draw.state.depthWrite);
    }
}

TEST_CASE("Yeti grab trail scrolls its subtree rather than the record's unrelated texture slot",
          "[game][world][effects][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/YETI/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    REQUIRE(effects.start(device, archive, "ATTACK11FXB", Vec3{0}));
    effects.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().state.uvScale == Vec2(0, 1));
    REQUIRE(device.draws.front().state.uvOffset == Vec2(0));
    effects.update(94.0f / 30);
    device.draws.clear();
    effects.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.front().state.uvScale == Vec2(0, 1));
    effects.update(5.0f / 30);
    device.draws.clear();
    effects.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.front().state.uvScale.x > 0);
    REQUIRE(device.draws.front().state.uvOffset.x < 0);
    effects.update(1);
    REQUIRE(effects.count() == 0);
}

TEST_CASE("Yeti frost breath preserves the staggered frame of each mist branch",
          "[game][world][effects][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/YETI/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    REQUIRE(effects.start(device, archive, "ATTACK3FX", Vec3{0}));
    effects.update(10.0f / 30);
    effects.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 18);
    // The first pair shares slot 11 but has animation delays of 1 and 9 frames.
    // All branches using the last node's delay would render the same texture.
    REQUIRE(device.draws[0].texture == &archive.textures.texture(device, 12 + 9));
    REQUIRE(device.draws[1].texture == &archive.textures.texture(device, 12 + 1));
    REQUIRE(device.draws[0].texture != device.draws.back().texture);
}

TEST_CASE("a node-attached effect draws with the moving parent's full basis",
          "[game][world][effects][unpacked]") {
    const auto root = test::unpackedOrSkip("WEAPONS/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    const auto id = effects.startSet(device, archive, "MP_FIRE", Vec3{0}, {});
    REQUIRE(id != 0);
    effects.draw(device, Mat4{1}, {});
    const auto local = device.draws;
    REQUIRE_FALSE(local.empty());
    for (const f32 angle : {0.6f, -1.2f}) {
        const Mat4 parent =
            glm::rotate(glm::translate(Mat4{1}, Vec3{4, 7, 9}), angle, Vec3{1, 0, 0});
        effects.placeAt(id, parent);
        REQUIRE(effects.effect(0).transform() == parent);
        device.draws.clear();
        effects.draw(device, Mat4{1}, {});
        REQUIRE(device.draws.size() == local.size());
        f32 error = 0;
        for (usize d = 0; d < local.size(); ++d) {
            REQUIRE(device.draws[d].vertices.size() == local[d].vertices.size());
            for (usize v = 0; v < local[d].vertices.size(); ++v) {
                const Vec3 expected{parent * Vec4{local[d].vertices[v].position, 1}};
                error =
                    std::max(error, glm::distance(device.draws[d].vertices[v].position, expected));
            }
        }
        REQUIRE(error < 0.001f);
    }
}

TEST_CASE("legend effects carry a world-space trail and draw its sprites facing the camera",
          "[game][world][effects][unpacked]") {
    const auto root = test::unpackedOrSkip("ITEMS/LEVELB6/animations.json").parent_path();
    ItemArchive items;
    REQUIRE(items.load(root));
    test::FakeRenderDevice device;
    test::FakeTexture particleTexture{1, 1};
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.seconds = 6.0f;
    setting.velocity = Vec3{20.0f, 0.0f, 0.0f};
    setting.unlit = true;
    setting.depthWrite = false;
    const u32 id = effects.startSet(device, items, "LEGENDPRJ", Vec3{0.0f}, setting);
    REQUIRE(id != 0);
    effects.attachTrail(id, LegendShow::trailOf(34), particleTexture);
    effects.update(1.0f / 30.0f);
    const ParticleEmitter& emitter = effects.effect(0).trails.emitter(0);
    REQUIRE(emitter.particles().size() == 1);
    const Vec3 origin = emitter.particles()[0].origin;
    effects.update(1.0f / 30.0f);
    REQUIRE(emitter.particles().size() == 2);
    REQUIRE(emitter.particles()[0].origin == origin);
    REQUIRE(emitter.particles()[1].origin.x > origin.x);
    CameraFrame camera;
    camera.right = Vec3{0.0f, 0.0f, 1.0f};
    effects.draw(device, Mat4{1.0f}, {}, &camera);
    REQUIRE_FALSE(device.draws.empty());
    const auto& particles = device.draws.back();
    REQUIRE(particles.texture == &particleTexture);
    REQUIRE_FALSE(particles.state.depthWrite);
    REQUIRE(particles.vertices.size() == 12);
    REQUIRE(particles.vertices[1].position - particles.vertices[0].position == camera.right * 2.0f);
    effects.stop(id);
    REQUIRE(effects.count() == 0);
    device.draws.clear();
    effects.draw(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.empty());
}

TEST_CASE("dragon FIRE plays both authored particle nodes without requiring a mesh",
          "[game][world][effects][breath][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/DRAGON/animations.json").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    const auto id = effects.startSet(device, archive, "FIRE", Vec3{0}, {});
    REQUIRE(id != 0);
    REQUIRE(effects.effect(0).particles.field().size() == 2);
    const Mat4 parent = glm::rotate(glm::translate(Mat4{1}, Vec3{4, 8, 12}), 0.6f, Vec3{1, 0, 0});
    effects.placeAt(id, parent);
    effects.update(1.0f / 30);
    const auto& effect = effects.effect(0);
    const auto& particles = effect.particles.field();
    REQUIRE(particles.particleCount() > 0);
    REQUIRE(particles.emitter(0).descriptor().texture == "DRAGONBREATH");
    REQUIRE(particles.emitter(1).descriptor().texture == "FBALLX");
    REQUIRE(particles.textureOf(0) != &device.whiteTexture());
    REQUIRE(particles.textureOf(1) != &device.whiteTexture());
    for (usize i = 0; i < particles.size(); ++i) {
        REQUIRE(particles.emitter(i).node() == parent * effect.pose.matrices()[i + 1]);
        REQUIRE(particles.emitter(i).descriptor().direction == effect.tree->nodes[i + 1].direction);
    }
    effects.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE_FALSE(device.draws.front().vertices.empty());
    // Frame replacement preserves the emitter; this tree has no texture-motion track.
    const auto textureSlot = archive.textures.find("DRAGONBREATH");
    REQUIRE(textureSlot.has_value());
    const test::FakeTexture frame{1, 1};
    // The public tree-particle binding also supports shared archive frame animations.
    TreeParticles replacement;
    replacement.bind(*effect.tree, archive, device, parent, effect.pose.matrices());
    replacement.step(1.0f / 30, parent, effect.pose.matrices());
    const auto count = replacement.field().particleCount();
    replacement.setTextureFrame(*textureSlot, frame);
    REQUIRE(replacement.field().textureOf(0) == &frame);
    REQUIRE(replacement.field().particleCount() == count);
    for (s32 i = 0; i < 15; ++i) {
        effects.update(1.0f / 30);
    }
    const Mat4 moved = glm::translate(parent, Vec3{2, 3, 4});
    effects.placeAt(id, moved);
    effects.update(1.0f / 30);
    REQUIRE(particles.emitter(0).node() == moved * effect.pose.matrices()[1]);
    for (s32 i = 0; i < 60; ++i) {
        effects.update(1.0f / 30);
    }
    REQUIRE(particles.particleCount() == 0); // template fades, not an invented endless flame
    effects.stop(id);
    REQUIRE_FALSE(effects.playing(id));
}

TEST_CASE("an effect tree plays its sequence once where it was started, then goes",
          "[game][world][effects][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("WEAPONS/animations.json").parent_path().parent_path();
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    REQUIRE_FALSE(effects.start(device, weapons, "NO_SUCH_TREE", Vec3{0.0f}));
    REQUIRE(effects.start(device, weapons, "MP_FIRE", Vec3{5.0f, 1.0f, -3.0f}, 0.5f));
    REQUIRE(effects.count() == 1);
    REQUIRE(effects.effect(0).name == "MP_FIRE");
    REQUIRE(effects.effect(0).scale == 0.5f);
    // Thirty-seven frames of a fifteenth of a second: drawn meanwhile, gone in two and a half.
    bool drew = false;
    s32 steps = 0;
    while (effects.count() > 0 && steps < 240) {
        effects.update(1.0f / 60.0f);
        device.draws.clear();
        effects.draw(device, Mat4{1.0f}, WorldLighting{});
        drew = drew || !device.draws.empty();
        ++steps;
    }
    REQUIRE(drew);
    REQUIRE(effects.count() == 0);
    REQUIRE(steps > 120);
    REQUIRE(steps < 180);
    effects.start(device, weapons, "MP_ACID", Vec3{0.0f});
    effects.clear();
    REQUIRE(effects.count() == 0);
}

TEST_CASE("a fast legend charge holds its final pose for the unscaled effect lifetime",
          "[game][world][effects][unpacked]") {
    const auto root = test::unpackedOrSkip("WEAPONS/animations.json").parent_path();
    ItemArchive weapons;
    REQUIRE(weapons.load(root));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.seconds = 1.0f;
    setting.loop = false;
    setting.playbackRate = LegendShow::kBurstPlaybackRate;
    const u32 id = effects.startSet(device, weapons, "COMBO_BLU", Vec3{0.0f}, setting);
    REQUIRE(id != 0);
    for (s32 i = 0; i < 30; ++i) {
        effects.update(1.0f / 60.0f);
    }
    REQUIRE(effects.playing(id));
    REQUIRE(effects.effect(0).player.finished());
    const f32 lastFrame = effects.effect(0).player.frame();
    effects.update(0.1f);
    REQUIRE(effects.effect(0).player.frame() == lastFrame);
    for (s32 i = 0; i < 30; ++i) {
        effects.update(1.0f / 60.0f);
    }
    REQUIRE_FALSE(effects.playing(id));
}

TEST_CASE("an effect can be turned, carried along and kept repeating until it is stopped",
          "[game][world][effects][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("WEAPONS/animations.json").parent_path().parent_path();
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.yaw = 1.0f;
    setting.velocity = Vec3{10.0f, 0.0f, 0.0f};
    setting.seconds = 30.0f; // far longer than the tree's one playing
    REQUIRE(effects.startSet(device, weapons, "NO_SUCH_TREE", Vec3{0.0f}, setting) == 0);
    const u32 id = effects.startSet(device, weapons, "MP_FIRE", Vec3{0.0f, 1.0f, 0.0f}, setting);
    REQUIRE(id != 0);
    for (s32 i = 0; i < 600; ++i) {
        effects.update(1.0f / 60.0f);
    }
    REQUIRE(effects.count() == 1); // still going, ten seconds on
    REQUIRE(effects.effect(0).position.x > 99.0f);
    REQUIRE(effects.effect(0).yaw == 1.0f);
    effects.stop(id);
    REQUIRE(effects.count() == 0);
    // Left to itself it goes when its time is up.
    setting.seconds = 0.5f;
    REQUIRE(effects.startSet(device, weapons, "MP_FIRE", Vec3{0.0f}, setting) != 0);
    for (s32 i = 0; i < 40; ++i) {
        effects.update(1.0f / 60.0f);
    }
    REQUIRE(effects.count() == 0);
}

TEST_CASE("an effect played through gives way to the tree named to take over, by name",
          "[game][world][effects][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("WEAPONS/animations.json").parent_path().parent_path();
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.seconds = 5.0f;
    setting.then = "MP_ACID";
    REQUIRE(effects.startSet(device, weapons, "MP_FIRE", Vec3{0.0f}, setting) != 0);
    REQUIRE(effects.effect(0).name == "MP_FIRE");
    for (s32 i = 0; i < 180; ++i) {
        effects.update(1.0f / 60.0f); // the fire's two and a half seconds
    }
    REQUIRE(effects.count() == 1);
    REQUIRE(effects.effect(0).name == "MP_ACID");
    REQUIRE(effects.effect(0).repeats);
    for (s32 i = 0; i < 150; ++i) {
        effects.update(1.0f / 60.0f);
    }
    REQUIRE(effects.count() == 0); // the five seconds are up
    // The crypt's book of protection burns on the lich as a tree with a sequence of no
    // frames, which stays its whole time.
    const std::filesystem::path crypt = root / "ITEMS" / "LEVELG5";
    if (std::filesystem::exists(crypt / "animations.json")) {
        ItemArchive items;
        REQUIRE(items.load(crypt));
        setting.then = "LEGENDFX";
        REQUIRE(effects.startSet(device, items, "LEGENDPRJ", Vec3{0.0f}, setting) != 0);
        for (s32 i = 0; i < 60; ++i) {
            effects.update(1.0f / 60.0f);
        }
        REQUIRE(effects.count() == 1);
        REQUIRE(effects.effect(0).name == "LEGENDFX");
        for (s32 i = 0; i < 180; ++i) {
            effects.update(1.0f / 60.0f);
        }
        REQUIRE(effects.count() == 1);
        for (s32 i = 0; i < 90; ++i) {
            effects.update(1.0f / 60.0f);
        }
        REQUIRE(effects.count() == 0);
    }
}

TEST_CASE("the classes' turbo effects play through, flip-books that start late and all",
          "[game][world][effects][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("PLAYERS/WIZ/SFXBLU/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    for (const char* cls : {"WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES"}) {
        ItemArchive archive;
        REQUIRE(archive.load(root / "PLAYERS" / cls / "SFXBLU"));
        for (usize t = 0; t < archive.trees.size(); ++t) {
            const std::string name = archive.trees.tree(static_cast<u32>(t)).name;
            CAPTURE(cls, name);
            EffectTrees effects;
            if (!effects.start(device, archive, name, Vec3{0.0f})) {
                continue; // a tree with nothing to bind
            }
            for (s32 i = 0; i < 400 && effects.count() > 0; ++i) {
                effects.update(1.0f / 60.0f);
                effects.draw(device, Mat4{1.0f}, WorldLighting{});
            }
            REQUIRE(effects.count() == 0);
        }
    }
}

} // namespace
