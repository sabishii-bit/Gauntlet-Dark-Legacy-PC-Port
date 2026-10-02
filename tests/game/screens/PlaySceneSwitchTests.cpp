#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/enemies/Enemies.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the Fields elevators carry a standing player through the gameplay loop",
          "[platform-contact][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    context.levels = &catalog;
    PlayOptions options;
    options.welcome = false;
    const bool poisonField = GENERATE(false, true);
    options.position = poisonField ? Vec3{46.875f, 19.4f, -208.28125f}
                                   : Vec3{40.828125f, 10.0703125f, -88.390625f};
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    f32 low = scene.actor(0)->position().y;
    f32 high = low;
    for (s32 frame = 0; frame < 480; ++frame) {
        PlayScene::Inputs inputs{};
        if (!poisonField && frame >= 180 && scene.actor(0)->position().x < 48.5f) {
            const Vec3 toward = Vec3{48.59375f, 0, -85.15625f} - scene.actor(0)->position();
            const f32 heading = std::atan2(toward.x, toward.z) - scene.viewCamera().yaw;
            inputs[0].move = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1};
        }
        scene.update(1.0 / 60, inputs);
        low = std::min(low, scene.actor(0)->position().y);
        high = std::max(high, scene.actor(0)->position().y);
    }
    CAPTURE(low, high, scene.actor(0)->position().x, scene.actor(0)->position().z);
    CHECK(high - low > 7.5f);
    CHECK(high >= (poisonField ? 27.2f : 19.3f));
}

TEST_CASE("Temple switch shots letterbox the target while combat and controls wait",
          "[switch-camera][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("E1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    context.levels = &catalog;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{61.25f, 0.325f, 6.75f};
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    for (s32 frame = 0; frame < 180; ++frame) {
        scene.update(1.0 / 30, {});
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.switchCutscene().active());
    REQUIRE(scene.actor(0));

    // Activate E1's first pad through the same visitor path used by party movement.
    const std::array visitors{TriggerVisitor{.position = Vec3{65.25f, 0.125f, 6.75f}}};
    world.updateTriggers(1.0f / 30, visitors);
    scene.update(1.0 / 30, {});
    REQUIRE(scene.switchCutscene().active());
    CHECK_FALSE(scene.canPause(0));
    CHECK_FALSE(scene.switchCutscene().showing());
    CHECK(scene.switchCutscene().target() == 560);
    const Vec3 held = scene.actor(0)->position();
    const auto health = scene.actor(0)->save().health();
    const auto targets = scene.enemies().targets();
    REQUIRE_FALSE(targets.empty());
    const auto enemyCount = scene.enemies().count();
    const Mat4 wallBefore = world.scene().worldTransform(560);
    PlayScene::Inputs input{};
    input[0].move = MoveInput{Vec2{0, 1}, 1};
    input[0].menu.start = true;
    input[0].attack = true;
    input[0].usePotion = true;
    bool sawShot = false;
    for (s32 frame = 0; frame < 240 && scene.switchCutscene().active(); ++frame) {
        REQUIRE(scene.update(1.0 / 30, input) == PlayOutcome::Running);
        CHECK(scene.actor(0)->position() == held);
        CHECK(scene.actor(0)->save().health() == health);
        CHECK(scene.enemies().count() == enemyCount);
        for (const auto& target : targets) {
            CHECK(scene.enemies().positionOf(target.id) == target.base);
        }
        if (scene.switchCutscene().showing() && !sawShot) {
            sawShot = true;
            CHECK(scene.viewCamera().position == Vec3{-34.78125f, 19.2890625f, 0.09375f});
            CHECK(scene.viewCamera().yaw == Approx(-1.56240499f));
            device.draws.clear();
            scene.render(device, makeScreenProjection(640, 448), 640, 448);
            std::vector<Vec2> corners;
            for (const auto& draw : device.draws) {
                if (draw.texture == &device.whiteTexture() && !draw.vertices.empty() &&
                    std::ranges::all_of(draw.vertices,
                                        [](const auto& v) { return v.color == Color::black(); })) {
                    for (const auto& vertex : draw.vertices) {
                        corners.emplace_back(vertex.position.x, vertex.position.y);
                    }
                }
            }
            // Both quads can share one canvas batch; inspect their own vertices.
            for (const Vec2 expected : {Vec2{0, 0}, Vec2{512, 48}, Vec2{0, 304}, Vec2{512, 384}}) {
                CHECK(std::ranges::any_of(corners, [expected](const Vec2& corner) {
                    return corner.x == Approx(expected.x) && corner.y == Approx(expected.y);
                }));
            }
        }
    }
    CHECK(sawShot);
    CHECK_FALSE(scene.switchCutscene().active());
    CHECK(world.triggers().settled(560));
    CHECK(world.scene().worldTransform(560) != wallBefore);
    CHECK(scene.canPause(0));
    CHECK(scene.viewCamera().position == scene.camera().camera().position);
    input[0].menu.start = false;
    input[0].attack = false;
    input[0].usePotion = false;
    for (s32 frame = 0; frame < 30; ++frame) {
        scene.update(1.0 / 30, input);
    }
    CHECK(scene.actor(0)->position() != held);
    scene.close();
    CHECK_FALSE(scene.switchCutscene().active());
}
} // namespace
