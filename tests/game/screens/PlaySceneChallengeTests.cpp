#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/ClassData.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the secret coin hunt awards both participants and waits for its scroll",
          "[secret][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELS4/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("TEXT/scroll_e.rom");
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const s32 levelIndex = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    const auto ref = levels.byName(std::format("S{}", levelIndex + 1));
    REQUIRE(ref);
    test::assetOrSkip(ref->directory + "/WORLDS.PS2");
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayScene scene;
    const std::array party{PartyMember{0, CharacterSave{}}, PartyMember{2, CharacterSave{}}};
    PlayOptions options;
    options.position = world.startPoint(0)->position;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.challenge().state() == SecretChallenge::State::Collecting);
    REQUIRE(scene.challenge().totalCoins() > 0);
    const f32 startTime = scene.challenge().remaining();
    for (s32 frame = 0; frame < 240 && scene.spawning(); ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.challenge().remaining() == startTime);
    }
    REQUIRE_FALSE(scene.spawning());
    // Move the mutable fixture directly to each contact. This tests collection
    // and the complete reward/scroll/return flow, not pathfinding through the map.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* collector = const_cast<PlayerActor*>(scene.actor(0));
    REQUIRE(collector != nullptr);
    for (usize i = 0; i < world.placedItems().size(); ++i) {
        const auto& item = world.placedItems().item(i);
        if (item.subtype == 1 && item.visible && !item.taken) {
            collector->place(item.position);
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
    }
    CHECK(scene.challenge().coinsLeft() == 0);
    REQUIRE(scene.challenge().state() == SecretChallenge::State::Won);
    REQUIRE(scene.scroll().active());
    REQUIRE_FALSE(scene.pickups().cards().empty());
    for (const auto& card : scene.pickups().cards()) {
        CHECK(card.texture == "COINHUD");
    }
    const s32 reward = SecretChallenge::classFor(levelIndex);
    for (const auto& member : scene.party()) {
        CHECK(classUnlocked(reward, member.save.classUnlock));
        const auto restored = CharacterSave::fromJson(member.save.toJson());
        CHECK(classUnlocked(reward, restored.classUnlock));
        CHECK(scene.pickups().count(member.player).count ==
              scene.pickups().count(member.player).total);
        CHECK(scene.pickups().count(member.player).icon ==
              (reward == kSumnerClass ? "16_SUM" : std::format("16_{}COIN", classCode(reward))));
    }
    for (s32 frame = 0; frame < 120; ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.challenge().remaining() == 1);
    }
    PlayScene::Inputs accept;
    accept[0].menu.select = true;
    accept[2].menu.select = true;
    auto outcome = PlayOutcome::Running;
    for (s32 frame = 0; frame < 600 && outcome == PlayOutcome::Running; ++frame) {
        outcome = scene.update(1.0 / 30, accept);
    }
    CHECK(outcome == PlayOutcome::Travel);
    CHECK(scene.secretTravel());
    scene.close();
}

TEST_CASE("running out of secret-world time returns without unlocking the class",
          "[secret][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELS4/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto ref = levels.byName("S4");
    REQUIRE(ref);
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    GameContext context;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayScene scene;
    const std::array party{PartyMember{0, CharacterSave{}}};
    REQUIRE(scene.open(device, context, world, party));
    auto outcome = PlayOutcome::Running;
    for (s32 frame = 0; frame < 2400 && outcome == PlayOutcome::Running; ++frame) {
        outcome = scene.update(1.0 / 30, {});
    }
    CHECK(outcome == PlayOutcome::Travel);
    CHECK(scene.secretTravel());
    CHECK(scene.challenge().state() == SecretChallenge::State::Returning);
    CHECK(scene.challenge().remaining() == 0);
    CHECK(scene.challenge().coinsLeft() > 0);
    CHECK(scene.actor(0)->save().classUnlock == 0);
    scene.close();
}

TEST_CASE("a consumed secret portal stays gone when the party returns to the parent stage",
          "[secret][alpha-secret-warp][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELB2/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELS4/WORLDS.PS2");
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto ref = levels.byName("B2");
    REQUIRE(ref);
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.sounds = &sounds;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayOptions options;
    options.position = Vec3{-36.3671875f, 90.1640625f, -85.0234375f};
    PlayScene scene;
    const std::array party{PartyMember{0, CharacterSave{}}};
    REQUIRE(scene.open(device, context, world, party, options));
    auto outcome = PlayOutcome::Running;
    std::array<f32, 3200> samples{};
    for (s32 frame = 0; frame < 240 && outcome == PlayOutcome::Running; ++frame) {
        outcome = scene.update(1.0 / 30, {});
        mixer.mix(samples);
        sounds.update();
    }
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(scene.secretTravel());
    CHECK(scene.destination().name == "S4");
    const usize placed = world.placedItems().size();
    sounds.stopAll();
    mixer.mix(samples);
    sounds.update();
    scene.suspendForChallenge();
    // do_exit80077D38 still calls fn8009D258 for a zero-tick secret departure.
    // The parent owns its COMMON bank while the challenge loads; its warp cue
    // must survive the suspension that silences normal stage ambience.
    CHECK(sounds.voiceCount() == 1);
    mixer.mix(samples);
    CHECK(std::ranges::any_of(samples, [](f32 sample) { return std::abs(sample) > 1e-5f; }));
    SoundSet common;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    const auto warp = common.find("S_TUNNEL");
    REQUIRE(warp);
    CHECK(common.entry(*warp).id == 4);
    AudioMixer referenceMixer(48000);
    SoundPlayer reference(referenceMixer);
    reference.play(common.sequence(*warp), 127.0f / 255.0f);
    std::array<f32, 3200> expected{};
    referenceMixer.mix(expected);
    for (usize i = 0; i < samples.size(); ++i) {
        CHECK(samples[i] == Catch::Approx(expected[i]).margin(1e-6f));
    }
    auto returned = scene.party();
    returned[0].save.gold += 100;
    returned[0].save.classUnlock |= 1;
    scene.resumeFromChallenge(returned);
    CHECK_FALSE(scene.secretTravel());
    CHECK(world.placedItems().size() == placed);
    CHECK(scene.actor(0)->save().gold == returned[0].save.gold);
    CHECK(scene.actor(0)->save().classUnlock == 1);
    for (usize i = 0; i < scene.portals().size(); ++i) {
        if (scene.portals().portal(i).secret) {
            CHECK(scene.portals().portal(i).consumed);
        }
    }
    for (s32 frame = 0; frame < 30; ++frame) {
        CHECK(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    scene.close();
}
} // namespace
