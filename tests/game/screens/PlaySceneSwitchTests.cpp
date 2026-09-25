#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
