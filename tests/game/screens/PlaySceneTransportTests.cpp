#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the castle transporter moves a player without leaving the level or bouncing back",
          "[transporters][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("A1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    GameContext context;
    context.unpackedRoot = root;
    context.levels = &catalog;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{-70.84375f, 0.325f, 94.78125f};
    options.yaw = 0.7f;
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.transporters().size() == 4);
    REQUIRE(scene.transporters().pad(0).destination == 1);
    const Vec3 destination = scene.transporters().pad(1).position;
    bool arrived = false;
    for (s32 frame = 0; frame < 300; ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        const auto& actor = *scene.actor(0);
        if (std::abs(actor.position().x - destination.x) < 0.01f) {
            arrived = true;
        }
        if (arrived) {
            CHECK(actor.position().x == Catch::Approx(destination.x));
            CHECK(actor.position().z == Catch::Approx(destination.z));
            CHECK(actor.yaw() == Catch::Approx(0.7f));
        }
    }
    CHECK(arrived);
    scene.close();
    CHECK(scene.transporters().size() == 0);
}

TEST_CASE("a tower portal reads the sparse player's held direction even during a stationary attack",
          "[portals][alpha-portal-ready][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    test::assetOrSkip("audio/COMMON.vbk");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    // exitFlameOn observes an audio handle, so this occupancy control needs a
    // sound player; silent scenes cannot create that handle.
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.sounds = &sounds;
    context.unpackedRoot = root;
    context.levels = &catalog;
    context.tower = &world;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{45.1f, -6.5f, -112.7f};
    const std::array party{PartyMember{3, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.actor(3));
    REQUIRE_FALSE(scene.actor(0));
    PlayScene::Inputs inputs;
    inputs[3].strongAttack = true;
    inputs[3].move.magnitude = 1;
    bool departed = false;
    s32 stationaryFrames = 0;
    std::array<f32, 1600> samples{};
    for (s32 frame = 0; frame < 360; ++frame) {
        // Stay on the same portal while retaining nonzero directional intent.
        // The throw's authored movement lock must not count as released input.
        inputs[3].move.direction = Vec2{frame % 4 < 2 ? 1.0f : -1.0f, 0};
        const auto outcome = scene.update(1.0 / 60, inputs);
        mixer.mix(samples);
        sounds.update();
        stationaryFrames += scene.actor(3)->moving() ? 0 : 1;
        departed |= scene.leaving() || outcome != PlayOutcome::Running;
        if (departed) {
            break;
        }
    }
    CAPTURE(stationaryFrames, scene.actor(3)->position().x, scene.actor(3)->position().y,
            scene.actor(3)->position().z);
    CHECK(stationaryFrames >= 6);
    REQUIRE_FALSE(departed);
    REQUIRE(scene.exitFlameOn());
    inputs[3].move = {};
    bool travelled = false;
    for (s32 frame = 0; frame < 600 && !travelled; ++frame) {
        travelled = scene.update(1.0 / 60, inputs) == PlayOutcome::Travel;
        mixer.mix(samples);
        sounds.update();
    }
    CHECK(travelled);
    CHECK(scene.destination().name == "G1");
    scene.close();
}
} // namespace
