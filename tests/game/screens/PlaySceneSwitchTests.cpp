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

TEST_CASE("a player activates and crosses the first Underworld descending pillar",
          "[pillar-crossing][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELF1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("F1")));
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
    options.position = Vec3{58.75f, 5.9f, 89.875f};
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    // Isolate terrain traversal from the nearby generators and crowd collision.
    scene.enemies().close();
    scene.generators().clear();
    constexpr s32 kPillar = 55;
    const Vec3 destination = world.layout().worldPosition(kPillar);
    for (s32 frame = 0; frame < 900; ++frame) {
        PlayScene::Inputs inputs{};
        const Vec3 toward = destination - scene.actor(0)->position();
        if (frame > 540 && glm::length(Vec2{toward.x, toward.z}) > 1) {
            const f32 heading = std::atan2(toward.x, toward.z) - scene.viewCamera().yaw;
            inputs[0].move = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1};
        }
        scene.update(1.0 / 60, inputs);
    }
    const Vec3 position = scene.actor(0)->position();
    CAPTURE(position.x, position.y, position.z);
    REQUIRE(scene.runtime(0)->life == PlayerLife::Standing);
    CHECK(world.triggers().opened(kPillar));
    CHECK(glm::length(Vec2{position.x - destination.x, position.z - destination.z}) < 1.1f);
    const auto support = world.collision().floorAt(position, 1, 1);
    REQUIRE(support);
    CHECK(support->object == kPillar);
}

TEST_CASE("the Fields elevators carry a standing player through the gameplay loop",
          "[platform-contact][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
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

TEST_CASE("Temple switch shots hold enemy AI and input while existing player actions finish",
          "[switch-camera][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
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
    // Face west across the walkway, toward a nearby enemy rather than the wall.
    options.yaw = -1.5707963f;
    CharacterSave save;
    save.progress().inventory.potions = {1};
    const std::array party{PartyMember{0, save}};
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
    // damage_player refuses hits during the trigger camera, even from effects
    // that were already in flight before controls were disabled.
    scene.harm(0, 10, HurtKind::Blow);
    CHECK(scene.actor(0)->save().health() == health);
    const auto targets = scene.enemies().targets();
    REQUIRE_FALSE(targets.empty());
    const auto enemyCount = scene.enemies().count();
    const Mat4 wallBefore = world.scene().worldTransform(560);
    PlayScene::Inputs input{};
    input[0].move = MoveInput{Vec2{0, 1}, 1};
    input[0].menu.start = true;
    input[0].attack = true;
    input[0].usePotion = true;
    REQUIRE(scene.runtime(0)->figure);
    const bool finishingThrow = GENERATE(false, true);
    if (finishingThrow) {
        // Establish an in-flight action, rather than letting the held cutscene
        // input manufacture one. It must complete once and return to its stance.
        scene.runtime(0)->figure->animate(0, 1, 1.0f / 60, PlayerDeed::StrongAttack);
        REQUIRE(scene.runtime(0)->figure->animator().action() ==
                PlayerAnimator::Action::StrongThrow);
    }
    const auto& animator = scene.runtime(0)->figure->animator();
    std::vector<Mat4> previousPose(animator.pose().matrices().begin(),
                                   animator.pose().matrices().end());
    bool animatedPlayer = false;
    REQUIRE(scene.effects().count() > 0);
    const u32 effectId = scene.effects().effect(0).id;
    const f32 effectAge = scene.effects().effect(0).lived;
    bool advancedEffect = false;
    s32 throws = 0;
    const auto potions = scene.actor(0)->save().progress().inventory.potions.size();
    bool sawShot = false;
    for (s32 frame = 0; frame < 240 && scene.switchCutscene().active(); ++frame) {
        REQUIRE(scene.update(1.0 / 30, input) == PlayOutcome::Running);
        CHECK(scene.actor(0)->position() == held);
        CHECK(scene.actor(0)->save().health() == health);
        CHECK(scene.enemies().count() == enemyCount);
        CHECK(scene.actor(0)->save().progress().inventory.potions.size() == potions);
        CHECK_FALSE(animator.released());
        CHECK_FALSE(animator.potionUsed());
        throws += animator.strongReleased() ? 1 : 0;
        if (animator.strongReleased()) {
            // This close-range throw strikes an enemy in its first physics
            // update. A cutscene must not leave that weapon frozen in flight.
            CHECK(scene.missiles().count() == 0);
        }
        for (usize i = 0; i < scene.effects().count(); ++i) {
            const auto& effect = scene.effects().effect(i);
            advancedEffect |= effect.id == effectId && effect.lived > effectAge;
        }
        const std::vector<Mat4> pose(animator.pose().matrices().begin(),
                                     animator.pose().matrices().end());
        animatedPlayer |= pose != previousPose;
        previousPose = pose;
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
                        const Vec4 clip = draw.transform * Vec4{vertex.position, 1};
                        corners.emplace_back(clip.x / clip.w, clip.y / clip.w);
                    }
                }
            }
            // Widescreen bands are submitted in clip space, not virtual UI pixels.
            // Check actual projected coverage regardless of the canvas batching path.
            for (const Vec2 expected :
                 {Vec2{-1, -1}, Vec2{1, -0.75f}, Vec2{-1, 304.0f / 192.0f - 1.0f}, Vec2{1, 1}}) {
                CHECK(std::ranges::any_of(corners, [expected](const Vec2& corner) {
                    return corner.x == Approx(expected.x) && corner.y == Approx(expected.y);
                }));
            }
        }
    }
    CHECK(sawShot);
    CHECK(animatedPlayer);
    CHECK(throws == (finishingThrow ? 1 : 0));
    CHECK(advancedEffect);
    CHECK(scene.missiles().count() == 0);
    CHECK(animator.action() == PlayerAnimator::Action::Ready);
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
    CHECK(scene.missiles().count() == 0);
    scene.harm(0, 10, HurtKind::Blow);
    CHECK(scene.actor(0)->save().health() < health);
    scene.close();
    CHECK_FALSE(scene.switchCutscene().active());
}
} // namespace
