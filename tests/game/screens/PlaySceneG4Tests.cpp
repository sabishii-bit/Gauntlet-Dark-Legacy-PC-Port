#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/config/GameConfig.h"
#include "game/screens/PlayScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("G4 exit scenario walks both prerequisite switches and climbs the final stairs",
          "[game][screens][g4-scene-traversal][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG4/WORLDS.PS2").parent_path().parent_path().parent_path();
    const s32 hz = GENERATE(30, 60);
    CAPTURE(hz);
    const f64 seconds = 1.0 / static_cast<f64>(hz);
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/level-g4-exit-stairs.json");
    REQUIRE(scenario.level == "G4");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(scenario.level);
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &catalog;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, scenario.partyMembers(), scenario.tower));
    REQUIRE(scene.actor(0));
    REQUIRE(scene.runtime(0));
    REQUIRE(scene.animator(0));
    // This is an exit-area journey from the shipped scenario, not a whole-level
    // walkthrough. Do not teleport after entry, force switches, remove opponents,
    // or bypass fixture/camera resolution with direct PlayerActor movement.
    REQUIRE_FALSE(world.triggers().opened(607));
    REQUIRE_FALSE(world.triggers().opened(608));

    s32 inputFrames = 0;
    bool acknowledgedScroll = false;
    const auto step = [&](PlayScene::Inputs inputs) {
        if (scene.scroll().active()) {
            // SCROLL instance 384 lies on this approach. A real player must
            // acknowledge its pages before walking; keep that modal in the route.
            inputs = {};
            inputs[0].menu.select = inputFrames % (hz / 2) == 0;
            acknowledgedScroll |= inputs[0].menu.select;
        }
        ++inputFrames;
        REQUIRE(scene.update(seconds, inputs) == PlayOutcome::Running);
        REQUIRE(scene.runtime(0)->life == PlayerLife::Standing);
    };
    for (s32 frame = 0; frame < 10 * hz && (scene.spawning() || scene.animator(0)->entering());
         ++frame) {
        step({});
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.animator(0)->entering());

    const auto walk = [&](Vec2 goal) {
        for (s32 frame = 0; frame < 20 * hz; ++frame) {
            const Vec3 position = scene.actor(0)->position();
            const Vec2 delta = goal - Vec2{position.x, position.z};
            if (glm::length(delta) < 0.3f) {
                return;
            }
            const f32 heading = std::atan2(delta.x, delta.y) - scene.viewCamera().yaw;
            PlayScene::Inputs inputs{};
            inputs[0].move = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1};
            step(inputs);
        }
        const Vec3 position = scene.actor(0)->position();
        CAPTURE(goal.x, goal.y, position.x, position.y, position.z, scene.runtime(0)->floor.object,
                scene.scroll().active(), scene.switchCutscene().active(),
                world.triggers().opened(607), world.triggers().opened(608), scene.viewCamera().yaw,
                scene.camera().yaw(), scene.actor(0)->yaw(),
                static_cast<s32>(scene.animator(0)->action()));
        for (const auto& contact : scene.actor(0)->wallContacts()) {
            UNSCOPED_INFO("blocking object " << contact.object << " at " << contact.point.x << ","
                                             << contact.point.y << "," << contact.point.z);
        }
        FAIL("G4 gameplay input did not reach its waypoint");
    };
    const auto waitForSwitch = [&](s32 target) {
        for (s32 frame = 0; frame < 10 * hz &&
                            (!world.triggers().opened(target) || scene.switchCutscene().active());
             ++frame) {
            step({});
        }
        CAPTURE(target);
        REQUIRE(world.triggers().opened(target));
        REQUIRE_FALSE(scene.switchCutscene().active());
    };

    // The scenario begins by the eastern switch. The western switch is still
    // needed: exercise its approach instead of pre-opening both for a stair test.
    walk({27, -98});
    waitForSwitch(608);
    REQUIRE_FALSE(world.triggers().opened(607));
    walk({15, -94});
    walk({-6.5f, -97.125f});
    waitForSwitch(607);
    REQUIRE(world.triggers().opened(608));
    walk({15, -94});
    walk({8, -99.75f});
    walk({12.2265625f, -99.75f});
    walk({17.25f, -104.875f});
    CHECK(acknowledgedScroll);
    CHECK(scene.runtime(0)->floor.object == 608);
    CHECK_FALSE(scene.fallen(0));
    CHECK_FALSE(scene.leaving());
}

} // namespace
