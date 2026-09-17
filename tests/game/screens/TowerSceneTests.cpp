#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/players/CharacterSave.h"
#include "game/menu/ScrollBox.h"
#include "game/screens/GameContext.h"
#include "game/screens/TowerScene.h"
#include "game/world/TowerWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

std::filesystem::path unpackedRoot() {
    return test::unpackedOrSkip("LEVELS/LEVELL1/collision.json")
        .parent_path()
        .parent_path()
        .parent_path();
}

TEST_CASE("the party enters the tower at its entrance and walks under control",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    TowerWorld world;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const AssetLocator assets(GDL_TEST_ASSET_DIR);
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.sounds = &sounds;
    context.assets = &assets;
    context.tower = &world;
    context.unpackedRoot = root;

    CharacterSave save;
    save.name = "AB";
    save.character = 0; // a blue warrior
    save.color = 1;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    TowerScene scene;
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE(scene.open(device, context, world, party));
    REQUIRE(scene.isOpen());
    REQUIRE(world.built());
    REQUIRE(scene.actorCount() == 1);
    REQUIRE(scene.actor(1) == nullptr);
    // The level's music loops from its stream when the game's files are at hand.
    const bool hasStream = assets.find("STREAMS/tower.ads").has_value();
    REQUIRE((scene.music() != kNoSound) == hasStream);
    if (hasStream) {
        REQUIRE(sounds.isPlaying(scene.music()));
    }

    const PlayerActor* actor = scene.actor(0);
    REQUIRE(actor != nullptr);
    const WorldLocator* start = world.startPoint(0);
    REQUIRE(start != nullptr);
    REQUIRE(actor->position().x == Approx(start->position.x).margin(0.01f));
    REQUIRE(actor->position().z == Approx(start->position.z).margin(0.01f));
    REQUIRE(actor->position().y == Approx(start->position.y).margin(1.0f));
    // Facing into the tower: a half turn from the marker, which points back out of the door.
    REQUIRE(std::abs(std::remainder(actor->yaw() - (start->rotation.y + kPi), 2.0f * kPi)) < 1e-4f);
    REQUIRE(scene.camera().marker() >= 0);
    REQUIRE(scene.sumner().loaded());

    // A new party is welcomed: Sumner's scroll holds the tower still, page by page on the
    // button after each page's hold.
    TowerScene::Inputs inputs{};
    inputs[0].move = MoveInput{Vec2{0.0f, 1.0f}, 1.0f};
    REQUIRE(scene.intro() == TowerScene::Intro::Scroll);
    const ScrollBox& scroll = scene.scroll();
    REQUIRE(scroll.active());
    REQUIRE(scroll.pageCount() == 5);
    // The crystals wait unseen for Sumner to reveal them.
    REQUIRE(world.placedItems().revealing());
    const Vec3 spawn = actor->position();
    const TowerScene::Inputs still{};
    TowerScene::Inputs accept{};
    accept[0].menu.select = true;
    // Back cannot leave the tower while the scroll is up.
    TowerScene::Inputs back{};
    back[0].menu.back = true;
    REQUIRE(scene.update(1.0 / 60.0, back) == TowerOutcome::Running);
    REQUIRE(scroll.active());
    for (usize page = 0; page < scroll.pageCount(); ++page) {
        REQUIRE(scroll.page() == page);
        for (int i = 0; i < 16; ++i) {
            REQUIRE(scene.update(1.0 / 60.0, inputs) == TowerOutcome::Running);
        }
        scene.update(1.0 / 60.0, accept);
    }
    REQUIRE(actor->position() == spawn);
    // The scroll burns away, then the camera cuts to the crystals as Sumner gestures, and the
    // party stays put for the cut's three hundred ticks.
    REQUIRE(scroll.burning());
    for (int i = 0; i < 60 && scene.intro() == TowerScene::Intro::Scroll; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.intro() == TowerScene::Intro::Crystal);
    scene.update(1.0 / 60.0, still); // the gesture cuts in on the next tick
    REQUIRE(scene.sumner().gesturing());
    REQUIRE(scene.viewCamera().position != scene.camera().camera().position);
    int heldTicks = 0;
    while (scene.intro() == TowerScene::Intro::Crystal && heldTicks < 400) {
        scene.update(1.0 / 60.0, inputs);
        ++heldTicks;
        REQUIRE(actor->position() == spawn);
    }
    REQUIRE(heldTicks >= TowerScene::kCrystalTicks - 1);
    REQUIRE(heldTicks <= TowerScene::kCrystalTicks);
    REQUIRE(scene.intro() == TowerScene::Intro::Done);
    // Over the cut every crystal has glowed in, the nearest first.
    REQUIRE_FALSE(world.placedItems().revealing());
    REQUIRE(scene.viewCamera().position == scene.camera().camera().position);

    // Half a second of walking forward moves the character and the camera follows.
    const Vec3 before = actor->position();
    const Vec3 cameraBefore = scene.camera().camera().position;
    for (int i = 0; i < 30; ++i) {
        REQUIRE(scene.update(1.0 / 60.0, inputs) == TowerOutcome::Running);
    }
    REQUIRE(glm::distance(actor->position(), before) > 1.0f);
    REQUIRE(glm::distance(scene.camera().camera().position, cameraBefore) > 0.1f);

    // The body played the class's entrance during the cut; a full stick runs.
    const PlayerAnimator* animator = scene.animator(0);
    REQUIRE(animator != nullptr);
    REQUIRE(animator->pose().size() == 26);
    usize mostVoices = sounds.voiceCount();
    for (int i = 0; i < 150; ++i) {
        scene.update(1.0 / 60.0, inputs);
        sounds.update();
        mostVoices = std::max(mostVoices, sounds.voiceCount());
    }
    REQUIRE((animator->action() == PlayerAnimator::Action::Run1 ||
             animator->action() == PlayerAnimator::Action::Run2));
    // Each half cycle of running sets a foot down on the stone.
    if (std::filesystem::exists(root / "audio/COMMON/sounds.json")) {
        REQUIRE(mostVoices > (hasStream ? 1U : 0U));
    }
    REQUIRE(scene.animator(1) == nullptr);

    scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
    REQUIRE(device.draws.size() > 10); // the level, the figures and the status boxes

    // A party with experience walks in without the welcome.
    CharacterSave veteran = save;
    veteran.progress().experience = 500;
    TowerScene again;
    REQUIRE(again.open(device, context, world, std::vector<PartyMember>{PartyMember{0, veteran}}));
    REQUIRE(again.intro() == TowerScene::Intro::None);
    again.close();

    TowerScene::Inputs leave{};
    leave[0].menu.back = true;
    REQUIRE(scene.update(1.0 / 60.0, leave) == TowerOutcome::Leave);
    const SoundHandle music = scene.music();
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE(scene.actorCount() == 0);
    REQUIRE(scene.music() == kNoSound);
    if (hasStream) {
        sounds.update();
        REQUIRE_FALSE(sounds.isPlaying(music));
    }
}

TEST_CASE("a scenario's options place the party and skip the welcome", "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    TowerWorld world;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const AssetLocator assets(GDL_TEST_ASSET_DIR);
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.sounds = &sounds;
    context.assets = &assets;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    const std::vector<PartyMember> party{PartyMember{0, save}};
    TowerOptions options;
    options.position = Vec3{19.3f, -2.0f, -62.0f}; // on one of the crystals
    options.yaw = 1.0f;
    options.welcome = false;
    TowerScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.intro() == TowerScene::Intro::None);
    const PlayerActor* actor = scene.actor(0);
    REQUIRE(actor != nullptr);
    REQUIRE(actor->position().x == Approx(19.3f));
    REQUIRE(actor->position().z == Approx(-62.0f));
    REQUIRE(actor->yaw() == Approx(1.0f));
    // Standing among the crystals, the party picks one up on the first step: it counts for
    // the first realm's gate.
    REQUIRE(actor->save().progress().crystals[1] == 0);
    const TowerScene::Inputs still{};
    for (int i = 0; i < 4; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(actor->save().progress().crystals[1] >= 1);
    REQUIRE(world.placedItems().visibleCount() < 15);
    REQUIRE_FALSE(world.placedItems().revealing()); // no welcome, nothing was hidden
    // The card rises over the taker's box and the count shows what the gate wants.
    REQUIRE(scene.pickups().cards().size() == 1);
    REQUIRE(scene.pickups().cards()[0].player == 0);
    REQUIRE(scene.pickups().cards()[0].texture == PickupHud::kCrystalCard);
    REQUIRE(scene.pickups().count(0).showing());
    REQUIRE(scene.pickups().count(0).count == actor->save().progress().crystals[1]);
    REQUIRE(scene.pickups().count(0).total == 15);
    // And a fresh party can be made to skip the welcome, or an old one to get it.
    scene.close();
    options.welcome = true;
    save.progress().experience = 500;
    const std::vector<PartyMember> veterans{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, veterans, options));
    REQUIRE(scene.intro() == TowerScene::Intro::Scroll);
    scene.close();
}

TEST_CASE("the tower scene refuses to open without the level", "[game][screens]") {
    const GameConfig config;
    test::FakeRenderDevice device;
    TowerWorld world;
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = test::scratchDirectory("tower-scene-none");
    TowerScene scene;
    REQUIRE_FALSE(scene.open(device, context, world, {}));
    REQUIRE_FALSE(scene.isOpen());
}

} // namespace
