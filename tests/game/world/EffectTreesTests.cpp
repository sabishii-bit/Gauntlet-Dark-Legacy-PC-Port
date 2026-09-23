#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/LegendItems.h"
#include "game/world/EffectTrees.h"

namespace {

using namespace gdl;
using namespace gdl::game;

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
    const std::uint32_t id = effects.startSet(device, items, "LEGENDPRJ", Vec3{0.0f}, setting);
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
    int steps = 0;
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
    const std::uint32_t id = effects.startSet(device, weapons, "COMBO_BLU", Vec3{0.0f}, setting);
    REQUIRE(id != 0);
    for (int i = 0; i < 30; ++i) {
        effects.update(1.0f / 60.0f);
    }
    REQUIRE(effects.playing(id));
    REQUIRE(effects.effect(0).player.finished());
    const float lastFrame = effects.effect(0).player.frame();
    effects.update(0.1f);
    REQUIRE(effects.effect(0).player.frame() == lastFrame);
    for (int i = 0; i < 30; ++i) {
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
    const std::uint32_t id =
        effects.startSet(device, weapons, "MP_FIRE", Vec3{0.0f, 1.0f, 0.0f}, setting);
    REQUIRE(id != 0);
    for (int i = 0; i < 600; ++i) {
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
    for (int i = 0; i < 40; ++i) {
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
    for (int i = 0; i < 180; ++i) {
        effects.update(1.0f / 60.0f); // the fire's two and a half seconds
    }
    REQUIRE(effects.count() == 1);
    REQUIRE(effects.effect(0).name == "MP_ACID");
    REQUIRE(effects.effect(0).repeats);
    for (int i = 0; i < 150; ++i) {
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
        for (int i = 0; i < 60; ++i) {
            effects.update(1.0f / 60.0f);
        }
        REQUIRE(effects.count() == 1);
        REQUIRE(effects.effect(0).name == "LEGENDFX");
        for (int i = 0; i < 180; ++i) {
            effects.update(1.0f / 60.0f);
        }
        REQUIRE(effects.count() == 1);
        for (int i = 0; i < 90; ++i) {
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
        for (std::size_t t = 0; t < archive.trees.size(); ++t) {
            const std::string name = archive.trees.tree(static_cast<std::uint32_t>(t)).name;
            CAPTURE(cls, name);
            EffectTrees effects;
            if (!effects.start(device, archive, name, Vec3{0.0f})) {
                continue; // a tree with nothing to bind
            }
            for (int i = 0; i < 400 && effects.count() > 0; ++i) {
                effects.update(1.0f / 60.0f);
                effects.draw(device, Mat4{1.0f}, WorldLighting{});
            }
            REQUIRE(effects.count() == 0);
        }
    }
}

} // namespace
