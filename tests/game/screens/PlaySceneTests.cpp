#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <set>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/config/GameConfig.h"
#include "game/enemies/Enemies.h"
#include "game/menu/ScrollBox.h"
#include "game/players/CharacterSave.h"
#include "game/players/PlayerControls.h"
#include "game/players/Progression.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelTriggers.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

f32 sumnerBeamAlpha(const LevelWorld& world) {
    const auto& objects = world.layout().objects();
    for (usize i = 0; i < objects.size(); ++i) {
        if (objects[i].name == "L1XPLOWERLIGHTR") {
            return world.objectAlpha(i);
        }
    }
    FAIL("Sumner's authored beam is missing");
    return -1;
}

bool awaitingEntrance(const PlayScene& scene) {
    if (scene.spawning()) {
        return true;
    }
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (scene.animator(player) != nullptr && scene.animator(player)->entering()) {
            return true;
        }
    }
    return false;
}

std::filesystem::path unpackedRoot() {
    return test::unpackedOrSkip("LEVELS/LEVELL1/collision.json")
        .parent_path()
        .parent_path()
        .parent_path();
}

TEST_CASE("a closed play scene has no per-player state", "[game][screens]") {
    PlayScene scene;
    for (const s32 player : {-1, 0, 1, 2, 3, 4}) {
        REQUIRE(scene.actor(player) == nullptr);
        REQUIRE(scene.animator(player) == nullptr);
        REQUIRE(scene.turboMeter(player) == nullptr);
        REQUIRE_FALSE(scene.fallen(player));
        REQUIRE_FALSE(scene.weaponHeld(player));
        REQUIRE_FALSE(scene.figureDirectory(player).has_value());
    }
    REQUIRE_FALSE(scene.figureDirectory(usize{0}).has_value());
    REQUIRE(scene.party().empty());
    scene.close();
    scene.close();
    REQUIRE(scene.actorCount() == 0);
}

TEST_CASE("Temple floor pickups can be switched off through mapped D-pad presses",
          "[game][screens][temple-selector][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("PLAYERS/JES/GRE/animations.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("E1");
    REQUIRE(ref.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    REQUIRE(world.startPoint(0) != nullptr);
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.character = 7;
    save.color = 3;
    save.progress().experience = levelExperience(60);
    PlayOptions options;
    options.welcome = false;
    options.position = world.startPoint(0)->position;
    options.items = {{"ELECICON", *options.position}, {"XRAYICON", *options.position}};
    const std::array party{PartyMember{0, save}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    for (s32 i = 0; i < 180; ++i) {
        scene.update(1.0 / 30, {});
    }
    const auto& inventory = scene.actor(0)->save().progress().inventory;
    REQUIRE(inventory.powerup(powerup::kWeapon, 2) != nullptr);
    REQUIRE(inventory.powerup(powerup::kSpecial, 2) != nullptr);
    Input input;
    PlayerControlReader controls;
    const auto step = [&](bool up) {
        input.beginPoll();
        PadSnapshot pad;
        pad.connected = true;
        pad.buttons[static_cast<usize>(PadButton::DpadUp)] = up;
        input.setPad(0, pad);
        const auto buttons = controls.read(input, config.play, false, 0, 1.0f / 30);
        PlayScene::Inputs inputs;
        inputs[0].selector = {buttons.selectorUp, buttons.selectorDown, buttons.selectorLeft,
                              buttons.selectorRight};
        scene.update(1.0 / 30, inputs);
    };
    step(true);
    for (s32 i = 0; i < 40; ++i) {
        step(false);
    }
    REQUIRE(scene.selector(0).showing());
    const auto selected = static_cast<usize>(scene.selector(0).selection());
    REQUIRE(inventory.powerups[selected].on);
    step(true);
    CHECK_FALSE(inventory.powerups[selected].on);
    step(false);
    step(true);
    CHECK(inventory.powerups[selected].on);
}

TEST_CASE("sparse party ids keep their state together across harm and scene reopening",
          "[game][screens][game-over][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName("G1");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level() != nullptr);
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave first;
    first.name = "THREE";
    first.color = 1;
    first.progress().health = 300;
    first.gold = 40;
    CharacterSave second = first;
    second.name = "ONE";
    second.progress().health = 700;
    second.gold = 90;
    const std::vector<PartyMember> party{PartyMember{3, first, 7, false, 80.0f, {9, 4}},
                                         PartyMember{1, second, 2, false, 20.0f, {6}}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{13.2f, 10.2f, -59.0f};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.actorCount() == 2);
    REQUIRE(scene.actor(0) == nullptr);
    REQUIRE(scene.actor(2) == nullptr);
    REQUIRE(scene.actor(3) != nullptr);
    REQUIRE(scene.actor(1) != nullptr);
    REQUIRE(scene.actor(3)->save().name == "THREE");
    REQUIRE(scene.actor(1)->save().name == "ONE");
    REQUIRE(scene.turboMeter(3)->held() == Approx(80.0f));
    REQUIRE(scene.turboMeter(1)->held() == Approx(20.0f));
    REQUIRE(scene.figureDirectory(usize{0}) == scene.figureDirectory(s32{3}));
    REQUIRE(scene.figureDirectory(usize{1}) == scene.figureDirectory(s32{1}));

    const auto positionBeforeSave = scene.actor(3)->position();
    const auto characterBeforeSave = scene.actor(3)->save().toJson();
    scene.setSaveSlot(3, 5);
    scene.setSaveSlot(0, 6); // absent input id is not a vector index
    CHECK(scene.party()[0].slot == 5);
    CHECK(scene.party()[1].slot == 2);
    CHECK(scene.actor(3)->position() == positionBeforeSave);
    CHECK(scene.actor(3)->save().toJson() == characterBeforeSave);
    scene.setSaveSlot(3, 7);

    scene.hurtPlayer(3, 100.0f, HurtKind::Blow);
    REQUIRE(scene.actor(3)->save().health() == 201); // less a point of the class's armour
    REQUIRE(scene.actor(1)->save().health() == 700);
    REQUIRE_FALSE(scene.fallen(1));
    const auto expectedExperience =
        second.experience() + static_cast<s32>(250.0f * world.level()->tuning.experienceScale(
                                                            experienceLevel(second.experience())));
    scene.awardExperience(1, 250);
    REQUIRE(scene.actor(1)->save().experience() == expectedExperience);
    REQUIRE(scene.actor(3)->save().experience() == first.experience());
    scene.awardExperience(1, 0, true); // zero-XP kills still contribute to the end-level tally
    scene.awardExperience(1, 0, false);
    REQUIRE(scene.levelResults().size() == 2);
    REQUIRE(scene.levelResults()[1].player == 1);
    REQUIRE(scene.levelResults()[1].totals[1] == 2);
    REQUIRE(scene.levelResults()[1].totals[2] == expectedExperience - second.experience());
    scene.hurtPlayer(3, 1000.0f, HurtKind::Burn);
    REQUIRE(scene.fallen(3));
    REQUIRE_FALSE(scene.fallen(1));
    REQUIRE(scene.levelResults().size() == 1);
    REQUIRE(scene.levelResults()[0].player == 1);
    REQUIRE(scene.turboMeter(3)->held() == 0.0f);
    REQUIRE(scene.turboMeter(1)->held() > 20.0f);
    const auto carried = scene.party();
    REQUIRE(carried.size() == 2);
    REQUIRE(carried[0].player == 3);
    REQUIRE(carried[0].slot == std::optional<usize>{7});
    REQUIRE(carried[0].fallen);
    REQUIRE(carried[0].save.health() == 300); // death carries the entry save, not the wounded one
    REQUIRE(carried[0].save.gold == 40);
    REQUIRE(carried[0].helpHeard == std::vector<s32>{4, 9});
    REQUIRE(carried[1].player == 1);
    REQUIRE(carried[1].slot == std::optional<usize>{2});
    REQUIRE_FALSE(carried[1].fallen);
    REQUIRE(carried[1].save.experience() == expectedExperience);
    REQUIRE(carried[1].helpHeard == std::vector<s32>{6});

    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 300; ++i) {
        REQUIRE(scene.update(1.0 / 60.0, still) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.gameOver().active());
    REQUIRE_FALSE(scene.canPause(3));
    REQUIRE(scene.canPause(1));
    REQUIRE_FALSE(scene.canJoin(0)); // only the tower takes anyone new
    scene.hurtPlayer(1, 10000.0f, HurtKind::Burn);
    REQUIRE_FALSE(scene.gameOver().active());
    REQUIRE_FALSE(scene.canPause(1));
    // Everyone fallen and the last death played out, the party goes back to the tower as
    // it came in: no defeat caption (game_main's all-dead case).
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 i = 0; i < 1200 && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
    }
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(scene.destination().isTower());
    REQUIRE_FALSE(scene.gameOver().active());
    REQUIRE(scene.levelResults().empty());
    // Quitting the game is what plays the defeat sequence.
    scene.startGameOver();
    REQUIRE(scene.gameOver().active());
    REQUIRE_FALSE(scene.canPause(0));
    for (s32 i = 0; i < GameOver::kDurationTicks - 1; ++i) {
        REQUIRE(scene.update(1.0 / 60.0, still) == PlayOutcome::Running);
    }
    REQUIRE(scene.update(1.0 / 60.0, still) == PlayOutcome::GameOver);
    REQUIRE(scene.party()[0].save.health() == 300);
    REQUIRE(scene.party()[1].save.health() == 700);
    REQUIRE(scene.party()[0].slot == std::optional<usize>{7});
    REQUIRE(scene.party()[1].slot == std::optional<usize>{2});

    // Reopening implicitly closes the old scene. No death, reaction, slot or turbo leaks.
    CharacterSave replacement = first;
    replacement.name = "ZERO";
    replacement.progress().health = 400;
    const std::vector<PartyMember> next{PartyMember{0, replacement, std::nullopt, false, 5.0f, {}}};
    REQUIRE(scene.open(device, context, world, next, options));
    REQUIRE(scene.actorCount() == 1);
    REQUIRE_FALSE(scene.gameOver().active());
    REQUIRE(scene.actor(3) == nullptr);
    REQUIRE(scene.actor(1) == nullptr);
    REQUIRE(scene.animator(3) == nullptr);
    REQUIRE(scene.turboMeter(3) == nullptr);
    REQUIRE(scene.actor(0)->save().name == "ZERO");
    REQUIRE(scene.actor(0)->save().health() == 400);
    REQUIRE_FALSE(scene.fallen(0));
    REQUIRE(scene.turboMeter(0)->held() == 5.0f);
    REQUIRE_FALSE(scene.party()[0].slot.has_value());
    REQUIRE(scene.party()[0].helpHeard.empty());
    scene.close();
    REQUIRE(scene.actorCount() == 0);
    REQUIRE(scene.party().empty());
}

TEST_CASE("the party enters the tower at its entrance and walks under control",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelWorld world;
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
    PlayScene scene;
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
    // The stained-glass light over the door waits for the temple's shards.
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        if (world.layout().objects()[i].name == "L1XPUPPERLIGHTR") {
            REQUIRE(world.objectAlpha(i) == 0.0f);
        }
    }

    // A new party is welcomed: Sumner's scroll holds the tower still, page by page on the
    // button after each page's hold.
    PlayScene::Inputs inputs{};
    inputs[0].move = MoveInput{Vec2{0.0f, 1.0f}, 1.0f};
    // First the party materialises, held still under the level's title with its effect at its
    // feet, seen from the start camera holding at the entrance marker; the camera then rides
    // in to the follow camera and the scroll unrolls.
    const Vec3 spawn = actor->position();
    REQUIRE(scene.spawning());
    REQUIRE(scene.spawnEffectCount() == 1);
    REQUIRE(scene.intro() == PlayScene::Intro::None);
    REQUIRE(scene.startCamera().phase() == StartCamera::Phase::Hold);
    const std::optional<WorldCamera> entrance = world.entranceCamera();
    REQUIRE(entrance.has_value());
    REQUIRE(scene.viewCamera().position == entrance->position);
    REQUIRE(scene.viewCamera().position != scene.camera().camera().position);
    s32 spawnTicks = 0;
    for (; spawnTicks < 600 && scene.spawning(); ++spawnTicks) {
        REQUIRE(scene.update(1.0 / 60.0, inputs) == PlayOutcome::Running);
        REQUIRE(actor->position() == spawn);
        if (scene.startCamera().phase() == StartCamera::Phase::Hold) {
            REQUIRE(scene.viewCamera().position == entrance->position);
        }
        if (spawnTicks == 30) {
            // Held still, the body still plays its entrance.
            REQUIRE(scene.animator(0) != nullptr);
            REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::Start);
            REQUIRE(scene.animator(0)->player().frame() > 0.0f);
        }
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE(spawnTicks >= StartCamera::kHoldTicks);
    REQUIRE(spawnTicks < StartCamera::kHoldTicks + 120);
    REQUIRE(scene.viewCamera().position == scene.camera().camera().position);
    REQUIRE(scene.intro() == PlayScene::Intro::Scroll);
    const ScrollBox& scroll = scene.scroll();
    REQUIRE(scroll.active());
    REQUIRE(scroll.pageCount() == 5);
    // The crystals wait unseen for Sumner to reveal them.
    REQUIRE(world.placedItems().revealing());
    const PlayScene::Inputs still{};
    PlayScene::Inputs accept{};
    accept[0].menu.select = true;
    // Back cannot leave the tower while the scroll is up.
    PlayScene::Inputs back{};
    back[0].menu.back = true;
    REQUIRE(scene.update(1.0 / 60.0, back) == PlayOutcome::Running);
    REQUIRE(scroll.active());
    for (usize page = 0; page < scroll.pageCount(); ++page) {
        REQUIRE(scroll.page() == page);
        for (s32 i = 0; i < 16; ++i) {
            REQUIRE(scene.update(1.0 / 60.0, inputs) == PlayOutcome::Running);
        }
        scene.update(1.0 / 60.0, accept);
    }
    REQUIRE(actor->position() == spawn);
    // The scroll burns away, then the camera cuts to the crystals as Sumner gestures, and the
    // party stays put for the cut's three hundred ticks.
    REQUIRE(scroll.burning());
    for (s32 i = 0; i < 60 && scene.intro() == PlayScene::Intro::Scroll; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.intro() == PlayScene::Intro::Crystal);
    scene.update(1.0 / 60.0, still); // the gesture cuts in on the next tick
    REQUIRE(scene.sumner().gesturing());
    REQUIRE(scene.viewCamera().position != scene.camera().camera().position);
    s32 heldTicks = 0;
    while (scene.intro() == PlayScene::Intro::Crystal && heldTicks < 400) {
        scene.update(1.0 / 60.0, inputs);
        ++heldTicks;
        REQUIRE(actor->position() == spawn);
    }
    REQUIRE(heldTicks >= PlayScene::kCrystalTicks - 1);
    REQUIRE(heldTicks <= PlayScene::kCrystalTicks);
    REQUIRE(scene.intro() == PlayScene::Intro::Done);
    // Over the cut every crystal has glowed in, the nearest first; Sumner's beam stays dark
    // with the party far from him.
    REQUIRE_FALSE(world.placedItems().revealing());
    REQUIRE(sumnerBeamAlpha(world) == 0.0f);
    REQUIRE(scene.viewCamera().position == scene.camera().camera().position);

    // Half a second of walking forward moves the character and the camera follows.
    const Vec3 before = actor->position();
    const Vec3 cameraBefore = scene.camera().camera().position;
    for (s32 i = 0; i < 30; ++i) {
        REQUIRE(scene.update(1.0 / 60.0, inputs) == PlayOutcome::Running);
    }
    REQUIRE(glm::distance(actor->position(), before) > 1.0f);
    REQUIRE(glm::distance(scene.camera().camera().position, cameraBefore) > 0.1f);

    // The body played the class's entrance during the cut; a full stick runs.
    const PlayerAnimator* animator = scene.animator(0);
    REQUIRE(animator != nullptr);
    REQUIRE(animator->pose().size() == 26);
    usize mostVoices = sounds.voiceCount();
    for (s32 i = 0; i < 150; ++i) {
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
    PlayScene again;
    REQUIRE(again.open(device, context, world, std::vector<PartyMember>{PartyMember{0, veteran}}));
    REQUIRE(again.intro() == PlayScene::Intro::None);
    again.close();

    // Back does nothing in play: the tower is left through its own menus, never by a slip.
    PlayScene::Inputs leave{};
    leave[0].menu.back = true;
    REQUIRE(scene.update(1.0 / 60.0, leave) == PlayOutcome::Running);
    REQUIRE(scene.isOpen());
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

TEST_CASE("a scenario's options place the party and skip the welcome",
          "[game][screens][tower-lights][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelWorld world;
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
    PlayOptions options;
    options.position = Vec3{19.3f, -2.0f, -62.0f}; // on one of the crystals
    options.yaw = 1.0f;
    options.welcome = false;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.intro() == PlayScene::Intro::None);
    // Placed by the options, the party is seen from the follow camera from the first frame.
    REQUIRE(scene.spawning());
    REQUIRE_FALSE(scene.canJoin(1)); // nobody joins while the party arrives
    REQUIRE_FALSE(scene.startCamera().active());
    REQUIRE(scene.viewCamera().position == scene.camera().camera().position);
    const PlayerActor* actor = scene.actor(0);
    REQUIRE(actor != nullptr);
    REQUIRE(actor->position().x == Approx(19.3f));
    REQUIRE(actor->position().z == Approx(-62.0f));
    REQUIRE(actor->yaw() == Approx(1.0f));
    // A first-level character wears the first costume tier and holds the first weapon.
    REQUIRE(scene.figureDirectory(0).has_value());
    REQUIRE(scene.figureDirectory(0)->filename() == "YEL00");
    REQUIRE(scene.weaponHeld(0));
    // Far from Sumner his beam stays dark.
    REQUIRE(sumnerBeamAlpha(world) == 0.0f);
    // Standing among the crystals, the party picks one up on the first step: it counts for
    // the first realm's gate.
    REQUIRE(actor->save().progress().crystals[1] == 0);
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < PlayScene::kSpawnTicks + 4; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(actor->save().progress().crystals[1] >= 1);
    // Arrived in the tower, a player not in the party may join; one in it may not.
    REQUIRE_FALSE(scene.spawning());
    REQUIRE(scene.canJoin(1));
    REQUIRE_FALSE(scene.canJoin(0));
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
    REQUIRE(scene.spawning());
    for (s32 i = 0; i < PlayScene::kSpawnTicks; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.intro() == PlayScene::Intro::Scroll);
    scene.close();
    // The realms' ambience: standing by the Battlefield portal, its drums start to loop.
    options.welcome = false;
    options.position = Vec3{82.0f, 10.0f, 161.0f};
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.ambience().size() == 53);
    REQUIRE(scene.ambience().playingCount() == 0);
    scene.update(1.0 / 60.0, still);
    REQUIRE(scene.ambience().playingCount() >= 1);
    scene.close();
    REQUIRE(scene.ambience().size() == 0);
    // Standing at Sumner's lectern activates the authored beam pad after arrival.
    REQUIRE(scene.open(device, context, world, party, options));
    const Vec3 lectern = scene.sumner().position();
    scene.close();
    options.position = lectern + Vec3{2.0f, 0.0f, 2.0f};
    REQUIRE(scene.open(device, context, world, party, options));
    const auto& objects = world.layout().objects();
    const auto window = std::ranges::find(objects, "L1XPLIGHTRAY01", &WorldObject::name);
    REQUIRE(window != objects.end());
    const auto windowIndex = static_cast<usize>(window - objects.begin());
    REQUIRE(world.objectAlpha(windowIndex) == 0.0f);
    REQUIRE(sumnerBeamAlpha(world) == 0.0f);
    for (s32 i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(sumnerBeamAlpha(world) > 0.0f);
    REQUIRE(world.objectAlpha(windowIndex) == 0.0f); // Sumner cannot reveal the Temple's beam
    REQUIRE(world.placedItems().size() > 0);
    scene.close();
}

TEST_CASE("the tower tells a short party what a gate wants and congratulates a ready one",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelWorld world;
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
    PlayOptions options;
    options.welcome = false;
    const PlayScene::Inputs still{};
    // In the first realm's force field with no crystals: the scroll says what it wants.
    options.position = Vec3{22.0f, -1.9f, -76.0f};
    PlayScene scene;
    {
        const std::vector<PartyMember> party{PartyMember{0, save}};
        REQUIRE(scene.open(device, context, world, party, options));
        for (s32 i = 0; i < PlayScene::kSpawnTicks + 2 && !scene.scroll().active(); ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE(scene.scroll().active());
        std::string words;
        for (const std::string& line : scene.scroll().lines()) {
            words += line + " ";
        }
        REQUIRE(words.find("15 Orange Crystals") != std::string::npos);
        scene.close();
    }
    // With fourteen, taking the fifteenth opens the gate: congratulations, and remembered.
    save.progress().crystals[1] = 14;
    options.position = Vec3{19.3f, -2.0f, -62.0f};
    {
        const std::vector<PartyMember> party{PartyMember{0, save}};
        REQUIRE(scene.open(device, context, world, party, options));
        const PlayerActor* actor = scene.actor(0);
        REQUIRE(actor != nullptr);
        for (s32 i = 0; i < PlayScene::kSpawnTicks + 4 && !scene.scroll().active(); ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE(actor->save().progress().crystals[1] == 15);
        REQUIRE(scene.scroll().active());
        std::string words;
        for (const std::string& line : scene.scroll().lines()) {
            words += line + " ";
        }
        REQUIRE(words.find("Congratulations") != std::string::npos);
        REQUIRE((actor->save().progress().unlocked & 2U) != 0);
        // Sumner speaks over the scroll; leaving it burns it and cuts him off.
        const SoundHandle voice = scene.voice();
        REQUIRE(voice != kNoSound);
        REQUIRE(sounds.isPlaying(voice));
        PlayScene::Inputs accept{};
        accept[0].menu.select = true;
        for (s32 i = 0; i < 400 && scene.scroll().active() && !scene.scroll().burning(); ++i) {
            scene.update(1.0 / 60.0, i % 20 == 19 ? accept : still);
        }
        REQUIRE(scene.scroll().burning());
        REQUIRE(scene.voice() == kNoSound);
        REQUIRE_FALSE(sounds.isPlaying(voice));
        // With the scroll gone, walking on into the gate: the field hums as it thins before
        // the party and falls silent once it has gone. The stick is camera-relative, so the
        // walk first learns which way forward and right take the character.
        for (s32 i = 0; i < 200 && scene.scroll().active(); ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE_FALSE(scene.scroll().active());
        REQUIRE(scene.fieldSound() == kNoSound);
        const Vec3 gate{22.0f, -2.0f, -79.0f};
        auto step = [&](const Vec2& stick, s32 ticks) {
            PlayScene::Inputs walk{};
            walk[0].move = MoveInput{stick, 1.0f};
            const Vec3 from = actor->position();
            for (s32 i = 0; i < ticks; ++i) {
                scene.update(1.0 / 60.0, walk);
            }
            return actor->position() - from;
        };
        // The entrance, held under the scroll, plays out before the character is free.
        for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE_FALSE(awaitingEntrance(scene));
        const Vec3 forward = glm::normalize(step(Vec2{0.0f, 1.0f}, 6));
        const Vec3 right = glm::normalize(step(Vec2{1.0f, 0.0f}, 6));
        for (s32 i = 0; i < 900 && scene.fieldSound() == kNoSound; ++i) {
            const Vec3 to = gate - actor->position();
            step(glm::normalize(Vec2{glm::dot(to, right), glm::dot(to, forward)}), 1);
        }
        const SoundHandle hum = scene.fieldSound();
        REQUIRE(hum != kNoSound);
        REQUIRE(sounds.isPlaying(hum));
        for (s32 i = 0; i < 120 && scene.fieldSound() != kNoSound; ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE(scene.fieldSound() == kNoSound);
        REQUIRE_FALSE(sounds.isPlaying(hum));
        scene.close();
    }
    // At the lion statue's feet without the golden claws: told what the gate wants, while
    // the statue, whose chain of triggers starts on a spot right there, stays still.
    options.position = Vec3{3.4f, -11.7f, 10.3f};
    {
        const std::vector<PartyMember> party{PartyMember{0, save}};
        REQUIRE(scene.open(device, context, world, party, options));
        for (s32 i = 0; i < PlayScene::kSpawnTicks + 2 && !scene.scroll().active(); ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE(scene.scroll().active());
        std::string words;
        for (const std::string& line : scene.scroll().lines()) {
            words += line + " ";
        }
        REQUIRE(words.find("28 Golden Lion Claws") != std::string::npos);
        s32 statue = -1;
        const std::vector<WorldObject>& objects = world.layout().objects();
        for (usize i = 0; i < objects.size(); ++i) {
            if (objects[i].name == "L1GROUP276") {
                statue = static_cast<s32>(i);
            }
        }
        REQUIRE(statue >= 0);
        const auto track = world.worldAnimator().trackOf(statue);
        REQUIRE(track.has_value());
        REQUIRE(world.worldAnimator().held(*track));
        REQUIRE(world.worldAnimator().frame(*track) == 0.0f);
        scene.close();
    }
}

TEST_CASE("a character takes what lies in its way by the original's rules",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().inventory.keys = 8;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{19.3f, -2.0f, -50.0f};
    // A key ring, a ham, a green potion and some gold, all underfoot.
    for (const char* name : {"KEYRING", "HAM", "POT_GRE", "TREAS_GOLD"}) {
        options.items.push_back(DroppedItem{name, *options.position});
    }
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const usize placed = world.placedItems().size();
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < PlayScene::kSpawnTicks + 4; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const CharacterSave& now = scene.actor(0)->save();
    // One key fitted: the ring lies there still with two. At full health the ham stays too.
    REQUIRE(now.progress().inventory.keys == 9);
    REQUIRE(now.progress().inventory.nextPotion() == 4);
    REQUIRE(now.gold == 200);
    REQUIRE(now.health() == 500);
    usize lying = 0;
    for (usize i = placed - 4; i < placed; ++i) {
        const PlacedItems::Item& item = world.placedItems().item(i);
        lying += item.visible ? 1U : 0U;
        if (item.name == "KEYRING") {
            REQUIRE(item.visible);
            REQUIRE(item.value == 2);
        }
    }
    REQUIRE(lying == 2);
    REQUIRE(scene.pickups().cards().size() == 3); // the key, the potion, the gold
    scene.close();
}

TEST_CASE("in the fields a runestone is everyone's, a gargoyle piece the finder's, and a "
          "scroll is read where it lies",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    test::unpackedOrSkip("text/scroll_e.json");
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave first;
    first.name = "AB";
    CharacterSave second;
    second.name = "CD";
    second.character = 1;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f}; // open ground
    options.items.push_back(DroppedItem{"RUNEC2", *options.position});
    options.items.push_back(DroppedItem{"GARGEAGL", *options.position});
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, first}, PartyMember{1, second}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    const Relics& mine = scene.actor(0)->save().progress().relics;
    const Relics& theirs = scene.actor(1)->save().progress().relics;
    for (s32 i = 0; i < 400 && mine.runeCount() + theirs.runeCount() == 0; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // The fields' rune (the eighth, from one) is held by both; the eagle's piece by whoever
    // stood on it.
    REQUIRE(mine.hasRune(7));
    REQUIRE(theirs.hasRune(7));
    REQUIRE(mine.gargoylePieces[1] + theirs.gargoylePieces[1] == 1);
    REQUIRE_FALSE(scene.scroll().active());
    // Its scroll's third page, dropped underfoot, opens over the party and is gone.
    s32 record = -1;
    const std::vector<ItemInfo>& infos = world.layout().itemInfos();
    for (usize i = 0; i < infos.size(); ++i) {
        if (infos[i].name == "SCROLL") {
            record = static_cast<s32>(i);
        }
    }
    REQUIRE(record >= 0);
    const Relics before = mine;
    REQUIRE(world.placeItemRecord(device, record, scene.actor(0)->position(), 3));
    const usize placed = world.placedItems().size();
    for (s32 i = 0; i < 60 && !scene.scroll().active(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.scroll().active());
    REQUIRE(scene.scroll().pageCount() == 1);
    REQUIRE_FALSE(scene.scroll().lines().empty());
    REQUIRE(scene.scroll().lines().front().starts_with("Death awaits"));
    REQUIRE(world.placedItems().item(placed - 1).taken);
    REQUIRE(mine == before); // nothing kept of it
    scene.close();
}

TEST_CASE("tower play does not consume the first level's gameplay lessons",
          "[game][screens][travel][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    const bool inTower = GENERATE(true, false);
    if (!inTower) {
        REQUIRE(world.load(device, root, *levels.byName("G1")));
    }
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(awaitingEntrance(scene));
    PlayScene::Inputs use{};
    use[0].usePotion = true;
    scene.update(1.0 / 60.0, use);
    const auto& seen = scene.actor(0)->save().helpSeen;
    CHECK((std::ranges::find(seen, HelpMessages::kNoPotion) != seen.end()) == !inTower);
    CHECK(scene.help().showing() == !inTower);
    scene.close();
}

TEST_CASE("the whole party on one of the tower's portals travels to the level it names",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.gold = 321;
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    {
        // The first of the portals to the Forsaken Province: its tag is g1.
        const std::vector<PartyMember> party{PartyMember{0, save}};
        options.position = Vec3{45.1f, -6.5f, -112.7f};
        REQUIRE(scene.open(device, context, world, party, options));
        REQUIRE(scene.portals().size() == 49);
        const PlayScene::Inputs still{};
        PlayOutcome outcome = PlayOutcome::Running;
        s32 leavingFrames = 0;
        f32 departureCameraY = 0;
        for (s32 i = 0; i < 900 && outcome == PlayOutcome::Running; ++i) {
            outcome = scene.update(1.0 / 60.0, still);
            leavingFrames += scene.leaving() ? 1 : 0;
            if (scene.leaving()) {
                bool raised = false;
                for (usize portal = 0; portal < scene.portals().size(); ++portal) {
                    const auto& exit = scene.portals().portal(portal);
                    raised |= exit.tag == "g1" && exit.action == ExitPortals::kWaiting;
                }
                CHECK(raised);
            }
            if (leavingFrames == 1) {
                departureCameraY = scene.camera().attention().y;
            }
            if (leavingFrames == 51) {
                CHECK(scene.camera().attention().y < departureCameraY - 3.0f);
            }
        }
        REQUIRE(outcome == PlayOutcome::Travel);
        // Fifty ticks of sinking/spinning precede the two-second covering transition.
        REQUIRE(leavingFrames >= 165);
        REQUIRE(leavingFrames <= 175);
        REQUIRE(scene.transition().covering());
        REQUIRE(scene.destination().name == "G1");
        REQUIRE(scene.destination().realmId == 7);
        REQUIRE(scene.party().size() == 1);
        REQUIRE(scene.party()[0].save.gold == 321); // what is carried goes along
    }
    // The fields load in the tower's place, without Sumner. Completing the stage
    // returns to the tower even when its authored next stage is unpacked.
    const std::vector<PartyMember> party = scene.party();
    const LevelRef fields = scene.destination();
    scene.close();
    REQUIRE(world.load(device, root, fields));
    REQUIRE_FALSE(world.isTower());
    REQUIRE(world.ref().title == "Fields");
    REQUIRE(world.level() != nullptr);
    REQUIRE(world.placedItems().size() > 100);
    PlayOptions arrive;
    arrive.welcome = true; // asked for, but only the tower welcomes anyone
    arrive.arrivalWorld = LevelRef::kTowerRealm;
    arrive.arriving = true;
    REQUIRE(scene.open(device, context, world, party, arrive));
    // The picture is up as the level opens and clears within the second.
    REQUIRE(scene.transition().showing());
    for (s32 i = 0; i < 60; ++i) {
        scene.update(1.0 / 60.0, PlayScene::Inputs{});
    }
    REQUIRE_FALSE(scene.transition().showing());
    REQUIRE_FALSE(scene.sumner().loaded());
    REQUIRE(scene.portals().size() == 1);
    REQUIRE(scene.portals().portal(0).tag == "g2");
    REQUIRE(scene.actor(0) != nullptr);
    const Vec3 entrance = scene.actor(0)->position();
    REQUIRE(glm::distance(entrance, Vec3{24.4f, 0.0f, 2.5f}) < 3.0f); // the level's start
    scene.close();
    PlayOptions atExit;
    atExit.welcome = false;
    atExit.position = Vec3{118.3f, 86.3f, -472.5f};
    REQUIRE(scene.open(device, context, world, party, atExit));
    const PlayScene::Inputs still{};
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 i = 0; i < 900 && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
    }
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(scene.destination().isTower());
    scene.close();
    REQUIRE(world.load(device, root)); // and the tower loads again after it
    REQUIRE(world.isTower());
}

TEST_CASE("Temple and Underworld exits descend directly to their boss arenas",
          "[game][screens][unpacked][portal-travel]") {
    const std::string stage = GENERATE(std::string{"E1"}, std::string{"F1"});
    const std::string arena = stage == "E1" ? "E2" : "F2";
    const auto root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVEL" + stage + "/world.json");
    test::unpackedOrSkip("LEVELS/LEVEL" + arena + "/world.json");
    test::unpackedOrSkip("audio/COMMON/sounds.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName(stage)));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.config = &config;
    context.sounds = &sounds;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    PlayOptions options;
    options.welcome = false;
    options.position = stage == "E1" ? Vec3{0.25f, 5.12f, -175.57f} : Vec3{-21.75f, 5.04f, -421.0f};
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.portals().size() == 1);
    REQUIRE(scene.portals().portal(0).destination->name == arena);
    bool heardFlame = false;
    s32 departing = 0;
    PlayOutcome outcome = PlayOutcome::Running;
    std::array<f32, 1600> samples{};
    for (s32 tick = 0; tick < 900 && outcome == PlayOutcome::Running; ++tick) {
        outcome = scene.update(1.0 / 60.0, {});
        heardFlame |= scene.exitFlameOn();
        if (scene.leaving()) {
            ++departing;
            CHECK_FALSE(scene.exitFlameOn()); // S_TUNNEL must not retain S_EXITFLAME
        }
        mixer.mix(samples);
        sounds.update();
    }
    REQUIRE(heardFlame);
    REQUIRE(departing >= 165);
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(scene.destination().name == arena);
    REQUIRE(scene.destination().index == 1);
    REQUIRE_FALSE(scene.destination().isTower()); // no after-level tally/shop interlude
}

TEST_CASE("in the fields a key opens a chest, which gives up what it held",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().inventory.keys = 1;
    PlayOptions options;
    options.welcome = false;
    // Beside the first chest a lone player sees, which holds a potion picked at random.
    options.position = Vec3{13.2f, 10.2f, -59.0f};
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save, 3}};
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.chests().size() == 28);
    REQUIRE(scene.gates().size() == 2);
    REQUIRE(scene.traps().size() == 25);
    const PlayScene::Inputs still{};
    const Inventory& carried = scene.actor(0)->save().progress().inventory;
    for (s32 i = 0; i < 400 && carried.potions.empty(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(carried.keys == 0);           // spent on the lock
    REQUIRE(carried.potions.size() == 1); // what the chest held, reached by touching it
    // Emptied, that chest has gone; the others stand in the way, nobody inside their boxes.
    bool opened = false;
    for (usize i = 0; i < scene.chests().size(); ++i) {
        const Chests::Chest& chest = scene.chests().chest(i);
        opened = opened || (chest.state == Chests::kOpen && chest.gone);
        if (chest.shown && !chest.gone) {
            REQUIRE(chest.box.pushOut(scene.actor(0)->position(), scene.actor(0)->radius()) ==
                    scene.actor(0)->position());
        }
    }
    REQUIRE(opened);
    // The party comes back out with what it gathered, each member still tied to its slot.
    const std::vector<PartyMember> after = scene.party();
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].slot == std::optional<usize>{3});
    REQUIRE(after[0].save.progress().inventory.potions.size() == 1);
    scene.close();

    // Walking into it opens it just the same, and nobody walking about ends up inside it.
    s32 openedWalking = 0;
    for (s32 heading = 0; heading < 8; ++heading) {
        CharacterSave walker;
        walker.name = "AB";
        walker.progress().inventory.keys = 1;
        options.position = Vec3{10.7f, 10.2f, -60.5f}; // open ground a few steps off
        const std::vector<PartyMember> alone{PartyMember{0, walker}};
        REQUIRE(scene.open(device, context, world, alone, options));
        PlayScene::Inputs walking{};
        const f32 angle = static_cast<f32>(heading) * 0.7853982f;
        walking[0].move.direction = Vec2{std::cos(angle), std::sin(angle)};
        walking[0].move.magnitude = 1.0f;
        for (s32 i = 0; i < 240; ++i) {
            scene.update(1.0 / 60.0, walking);
            const PlayerActor& walkerNow = *scene.actor(0);
            for (const Obstacle& box : scene.chests().obstacles()) {
                const Vec3 out = box.pushOut(walkerNow.position(), walkerNow.radius());
                REQUIRE(glm::length(out - walkerNow.position()) < 0.5f);
            }
        }
        openedWalking += scene.actor(0)->save().progress().inventory.keys == 0 ? 1 : 0;
        scene.close();
    }
    REQUIRE(openedWalking >= 1);
}

TEST_CASE("in the fields harm is the level's own: help is given, barrels break, the fallen wait",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    test::unpackedOrSkip("text/english.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    REQUIRE(world.level() != nullptr);
    REQUIRE(world.level()->tuning.trapDamage == 0.5f);
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.gold = 40;
    save.progress().health = 300;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{13.2f, 10.2f, -59.0f}; // against the locked chest, with no key
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save, 3}};
    REQUIRE(scene.open(device, context, world, party, options));
    // Its spikes do half what their record says, as the level scales them.
    bool spikes = false;
    for (usize i = 0; i < scene.traps().size(); ++i) {
        spikes = spikes || scene.traps().trap(i).damage == 10.0f;
    }
    REQUIRE(spikes);
    REQUIRE(scene.barrels().size() > 40);
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 300 && !scene.help().showing(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // Refused, the chest has the party told what it wants, once.
    REQUIRE(scene.help().showing());
    REQUIRE(scene.help().id() == HelpMessages::kChestNeedsKey);
    REQUIRE(scene.help().lines().size() == 2);
    REQUIRE(scene.actor(0)->save().helpSeen == std::vector<s32>{HelpMessages::kChestNeedsKey});

    // A blast breaks the barrels about it; the one by the first field gives up its key.
    usize holder = scene.barrels().size();
    for (usize i = 0; i < scene.barrels().size(); ++i) {
        const Breakables::Barrel& barrel = scene.barrels().barrel(i);
        if (barrel.kind == BreakableStrike::Kind::Holding && barrel.shown &&
            std::abs(barrel.figure.position().x + 42.8f) < 0.5f) {
            holder = i;
        }
    }
    REQUIRE(holder < scene.barrels().size());
    const usize lying = world.placedItems().size();
    const Vec3 at = scene.barrels().barrel(holder).figure.position();
    scene.blast(at, 3.0f, 30.0f);
    REQUIRE_FALSE(scene.barrels().standing(holder));
    REQUIRE(world.placedItems().size() == lying + 1);
    REQUIRE(world.placedItems().item(lying).name == "KEY");
    REQUIRE(scene.actor(0)->save().health() == 300); // far from it

    // Hurt, the character loses health; with none left it falls, and its box says where it
    // waits. The party goes on without what the level gave it.
    scene.hurtPlayer(0, 100.0f, HurtKind::Blow);
    REQUIRE(scene.actor(0)->save().health() == 201); // less a point of the class's armour
    REQUIRE_FALSE(scene.fallen(0));
    const auto white = world.powerups().textures.find("AAAWHITE");
    REQUIRE(white);
    const Texture* flash = &world.powerups().textures.texture(device, *white);
    const auto flashes = [&] {
        device.draws.clear();
        scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
        return std::ranges::any_of(device.draws, [&](const test::RecordedDraw& draw) {
            return draw.state.maskedTexture == flash;
        });
    };
    CHECK(flashes());
    for (s32 i = 0; i < 4; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    CHECK_FALSE(flashes());
    scene.hurtPlayer(0, 500.0f, HurtKind::Burn);
    REQUIRE(scene.fallen(0));
    scene.hurtPlayer(0, 500.0f, HurtKind::Burn); // the fallen are past hurting
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 i = 0; i < 1200 && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
    }
    REQUIRE(outcome == PlayOutcome::Travel); // a wiped party goes back to the tower
    REQUIRE(scene.destination().isTower());
    const std::vector<PartyMember> after = scene.party();
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].fallen);
    REQUIRE(after[0].slot == std::optional<usize>{3});
    REQUIRE(after[0].save.health() == 300); // as it came in
    REQUIRE(after[0].save.helpSeen == std::vector<s32>{HelpMessages::kChestNeedsKey});
    scene.close();
}

TEST_CASE("a fallen player's keys lie where they fell for the rest of the party",
          "[game][screens][keys][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    for (const s32 keys : {1, 3}) {
        CAPTURE(keys);
        CharacterSave save;
        save.name = "AB";
        save.progress().health = 300;
        save.progress().inventory.keys = keys;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{-36.2f, 26.5f, -125.3f}; // the death scenario's open ground
        PlayScene scene;
        const std::vector<PartyMember> party{PartyMember{0, save, 3}};
        REQUIRE(scene.open(device, context, world, party, options));
        const usize lying = world.placedItems().size();
        const PlayScene::Inputs still{};
        scene.hurtPlayer(0, 5000.0f, HurtKind::Burn);
        REQUIRE(scene.fallen(0));
        for (s32 i = 0; i < 600 && world.placedItems().size() == lying; ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE(world.placedItems().size() == lying + 1);
        const PlacedItems::Item& dropped = world.placedItems().item(lying);
        CHECK(dropped.name == (keys == 1 ? "KEY" : "KEYRING"));
        CHECK(dropped.value == keys);
        CHECK(glm::distance(Vec2{dropped.position.x, dropped.position.z}, Vec2{-36.2f, -125.3f}) <
              1.0f);
        CHECK(scene.actor(0)->save().progress().inventory.keys == 0);
        // Its save still goes back to how it came in.
        CHECK(scene.party()[0].save.progress().inventory.keys == keys);
        scene.close();
    }
}

TEST_CASE("the fallen are asked to wait in the tower or quit, and the last to quit ends the game",
          "[game][screens][tower-prompt][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave first;
    first.name = "AB";
    first.progress().health = 300;
    CharacterSave second = first;
    second.name = "CD";
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{-36.2f, 26.5f, -125.3f};
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, first, 3}, PartyMember{1, second, 2}};
    REQUIRE(scene.open(device, context, world, party, options));
    PlayScene::Inputs still{};
    const auto fall = [&](s32 player) {
        scene.hurtPlayer(player, 5000.0f, HurtKind::Burn);
        for (s32 i = 0; i < 600 && !scene.status(player).inTower; ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE(scene.status(player).inTower);
    };
    // The first falls: asked, and answers that it waits in the tower.
    fall(0);
    CHECK(scene.status(0).towerPrompt);
    PlayScene::Inputs wait{};
    wait[0].menu.select = true;
    scene.update(1.0 / 60.0, wait);
    CHECK_FALSE(scene.status(0).towerPrompt);
    CHECK(scene.status(0).inTower);
    CHECK(scene.party().size() == 2);
    // The second falls and quits: out of the party; with one still waiting, the game goes on.
    fall(1);
    PlayScene::Inputs quit{};
    quit[1].menu.back = true;
    scene.update(1.0 / 60.0, quit);
    CHECK_FALSE(scene.status(1).active);
    REQUIRE(scene.party().size() == 1);
    CHECK(scene.party()[0].player == 0);
    scene.close();

    // Alone, a player who quits ends the game (game_main's no active players).
    PlayScene alone;
    REQUIRE(alone.open(device, context, world, std::vector<PartyMember>{party[0]}, options));
    alone.hurtPlayer(0, 5000.0f, HurtKind::Burn);
    for (s32 i = 0; i < 600 && !alone.status(0).towerPrompt; ++i) {
        alone.update(1.0 / 60.0, still);
    }
    REQUIRE(alone.status(0).towerPrompt);
    PlayScene::Inputs leave{};
    leave[0].menu.back = true;
    PlayOutcome outcome = alone.update(1.0 / 60.0, leave);
    for (s32 i = 0; i < 600 && outcome == PlayOutcome::Running; ++i) {
        outcome = alone.update(1.0 / 60.0, still);
    }
    CHECK(outcome == PlayOutcome::GameOver);
    alone.close();
}

TEST_CASE("spikes make whoever they catch flinch where they stand", "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().health = 400;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{-36.2f, 26.5f, -125.3f}; // on a bed of spikes
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 600 && scene.actor(0)->save().health() == 400; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.actor(0)->save().health() == 391); // twenty, halved by the level, less armour
    // The tick after, the body flinches, and pushing the stick moves it nowhere until it is
    // over; then it walks off.
    PlayScene::Inputs walking{};
    walking[0].move.direction = Vec2{1.0f, 0.0f};
    walking[0].move.magnitude = 1.0f;
    scene.update(1.0 / 60.0, walking);
    REQUIRE(scene.animator(0) != nullptr);
    // Spikes play their own reaction (SPIKEHIT), not the ordinary flinch.
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::SpikeHit);
    const Vec3 struckAt = scene.actor(0)->position();
    s32 held = 0;
    while (scene.animator(0)->reacting() && held < 200) {
        REQUIRE(glm::distance(scene.actor(0)->position(), struckAt) < 0.05f);
        scene.update(1.0 / 60.0, walking);
        ++held;
    }
    REQUIRE(held > 10);
    REQUIRE(held < 200);
    for (s32 i = 0; i < 30; ++i) {
        scene.update(1.0 / 60.0, walking);
    }
    REQUIRE(glm::distance(scene.actor(0)->position(), struckAt) > 1.0f);
    scene.close();
}

TEST_CASE("a party back from a realm materialises among its portals, the camera on it",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    // The town's gate is met: a party whose realm is shut is dropped at the entrance.
    save.progress().crystals[1] = LevelTriggers::crystalsNeeded(1);
    PartyMember member{0, save};
    member.fallen = true; // it died out there: in the tower it stands again
    const std::vector<PartyMember> party{member};
    PlayOptions back;
    back.welcome = false;
    back.arriving = true;
    back.arrivalWorld = 7; // the town realm
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, back));
    const Vec3 ring{37.8f, -6.3f, -117.5f};
    REQUIRE(glm::distance(scene.actor(0)->position(), ring) < 3.0f);
    REQUIRE_FALSE(scene.fallen(0));
    // No ride in from the entrance hall: the view is the follow camera's, looking at them,
    // while they play their entrance under the level's title.
    REQUIRE_FALSE(scene.startCamera().active());
    REQUIRE(scene.spawning());
    REQUIRE(scene.viewCamera().position == scene.camera().camera().position);
    REQUIRE(scene.viewCamera().yaw == scene.camera().camera().yaw);
    REQUIRE(scene.viewCamera().pitch == scene.camera().camera().pitch);
    REQUIRE(glm::distance(scene.viewCamera().position, ring) < 60.0f);
    scene.update(1.0 / 60.0, PlayScene::Inputs{});
    REQUIRE(scene.animator(0) != nullptr);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::Start);
    scene.close();
    // At the tower's own entrance the start camera still holds and rides in.
    PlayOptions fresh;
    fresh.welcome = false;
    REQUIRE(scene.open(device, context, world, party, fresh));
    REQUIRE(scene.startCamera().active());
    scene.close();
}

TEST_CASE("what the level tells the party is in the string table's language",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    test::unpackedOrSkip("text/english.json");
    const auto dir = test::scratchDirectory("play-scene-language");
    writeTextFile(dir / "fr.json", R"({"help.usekeyopenchest.1": "IL FAUT UNE CLEF"})");
    StringTable strings;
    REQUIRE(strings.load(dir, "fr", "fr"));
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{13.2f, 10.2f, -59.0f}; // against the locked chest, with no key
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 300 && !scene.help().showing(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.help().id() == HelpMessages::kChestNeedsKey);
    REQUIRE(scene.help().lines() == std::vector<std::string>{"IL FAUT UNE CLEF"});
    scene.close();
}

TEST_CASE("the turbo meter climbs in play and its moves are paid for out of it",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    PartyMember member{0, save};
    member.turbo = 99.0f;
    const std::vector<PartyMember> party{member};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{3.0f, 2.0f, -20.0f};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.turboMeter(0) != nullptr);
    REQUIRE(scene.turboMeter(3) == nullptr);
    const PlayScene::Inputs still{};
    // It fills two points a second, its shown level chasing; full, the box glows.
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    for (s32 i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.turboMeter(0)->held() == TurboMeter::kFull);
    REQUIRE(scene.turboMeter(0)->shown() == TurboMeter::kFull);
    REQUIRE(scene.turboMeter(0)->flash() == TurboMeter::Flash::Glow);
    const StatusBoxView view = scene.status(0);
    REQUIRE(view.turbo.has_value());
    REQUIRE(view.turbo->front == Color::rgba(255, 0, 0));
    REQUIRE_FALSE(scene.status(2).turbo.has_value());

    // Drive the actual configurable reader: B held as A goes down buys the greater attack.
    Input physical;
    PlayerControlReader reader;
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::B)] = true;
    physical.setPad(0, pad);
    reader.read(physical, config.play, false, 0, 1.0f / 60.0f);
    physical.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    physical.setPad(0, pad);
    const PlayButtons buttons = reader.read(physical, config.play, false, 0, 1.0f / 60.0f);
    PlayScene::Inputs strike{};
    strike[0].turbo = buttons.turbo;
    strike[0].attack = buttons.attack;
    strike[0].turboAttackPressed = buttons.turboAttackPressed;
    scene.update(1.0 / 60.0, strike);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::TurboFull);
    // It is paid for as its first strike is made, which sets its harm going.
    for (s32 i = 0; i < 300 && scene.animator(0)->turboing(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.animator(0)->turboing());
    REQUIRE(scene.turboMeter(0)->held() < 5.0f);
    // With too little for either, turbo held is only the guard: no attack comes of it.
    scene.update(1.0 / 60.0, strike);
    REQUIRE(scene.animator(0)->guarding());
    REQUIRE_FALSE(scene.animator(0)->throwing());
    scene.close();

    // Two fifths buys the lesser attack; a twentieth, a shove, which runs it down.
    member.turbo = 45.0f;
    const std::vector<PartyMember> again{member};
    REQUIRE(scene.open(device, context, world, again, options));
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const f32 before = scene.turboMeter(0)->held();
    scene.update(1.0 / 60.0, strike);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::TurboStrong);
    for (s32 i = 0; i < 300 && scene.animator(0)->turboing(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.turboMeter(0)->held() < before - TurboMeter::kStrongCost + 0.1f);
    REQUIRE(scene.turboMeter(0)->held() > before - TurboMeter::kStrongCost - 0.1f);
    // Holding turbo by itself is the guard; the charge button is the charge's.
    PlayScene::Inputs holding{};
    holding[0].turbo = true;
    scene.update(1.0 / 60.0, holding);
    REQUIRE_FALSE(scene.animator(0)->turboing());
    REQUIRE(scene.animator(0)->guarding());
    for (s32 i = 0; i < 60 && scene.animator(0)->guarding(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // Experience won feeds the meter.
    const f32 fed = scene.turboMeter(0)->held();
    scene.awardExperience(0, 400);
    REQUIRE(scene.turboMeter(0)->held() > fed + 9.9f);
    REQUIRE(scene.actor(0)->save().experience() == 400);
    PlayScene::Inputs tap{};
    tap[0].chargePressed = true;
    const f32 left = scene.turboMeter(0)->held();
    REQUIRE(left >= TurboMeter::kShoveFrom);
    scene.update(1.0 / 60.0, tap);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::Shove);
    for (s32 i = 0; i < 20; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.turboMeter(0)->held() < left - 4.0f);
    scene.close();
}

TEST_CASE("in the fields a turbo attack breaks what is about it, a charge rams, a guard blocks",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    test::unpackedOrSkip("text/english.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().health = 400;
    PartyMember member{0, save};
    member.turbo = 60.0f;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{13.8f, 0.2f, 1.2f}; // four units from the barrel by the start
    options.yaw = kPi;                          // and facing it
    PlayScene scene;
    const std::vector<PartyMember> party{member};
    REQUIRE(scene.open(device, context, world, party, options));
    usize barrel = scene.barrels().size();
    for (usize i = 0; i < scene.barrels().size(); ++i) {
        const Vec3 at = scene.barrels().barrel(i).figure.position();
        if (scene.barrels().standing(i) && std::abs(at.x - 13.8f) < 0.5f &&
            std::abs(at.z + 2.8f) < 0.5f) {
            barrel = i;
        }
    }
    REQUIRE(barrel < scene.barrels().size());
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // The lesser turbo attack: owed until its strike is made, then its burst, half a second
    // on, breaks the barrel four units off.
    PlayScene::Inputs strike{};
    strike[0].turbo = true;
    strike[0].attack = true;
    strike[0].turboAttackPressed = true;
    const f32 before = scene.turboMeter(0)->held();
    scene.update(1.0 / 60.0, strike);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::TurboStrong);
    bool struck = false;
    f32 darkest = 0.0f;
    f32 brightest = 0.0f;
    for (s32 i = 0; i < 300 && scene.barrels().standing(barrel); ++i) {
        scene.update(1.0 / 60.0, still);
        struck = struck || scene.strikes().count() > 0;
        darkest = std::min(darkest, world.ambientOffset());
        brightest = std::max(brightest, scene.runtime(0)->glow.level());
    }
    REQUIRE(struck);
    // The striker blazes against the dark it brings (PlyrSfxDoDamage's pulse of 2).
    REQUIRE(brightest == BodyGlow::kStrike);
    // Its name was announced as it came out, and the party carries that on with it, so that
    // it is not said again next level.
    REQUIRE(scene.help().id() == 57);
    REQUIRE(scene.help().lines() == std::vector<std::string>{"FIRE ARC"});
    REQUIRE(scene.party()[0].helpHeard == std::vector<s32>{57});
    REQUIRE(darkest < -0.39f); // the level goes dark while the move comes out
    REQUIRE(darkest > -0.41f);
    REQUIRE(world.lighting().ambient.x < world.level()->ambient);
    REQUIRE_FALSE(scene.barrels().standing(barrel));
    REQUIRE(scene.turboMeter(0)->held() < before - TurboMeter::kStrongCost + 3.0f);
    REQUIRE(scene.actor(0)->save().health() == 400); // its own burst does it no harm
    for (s32 i = 0; i < 240; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(world.ambientOffset() == 0.0f); // and light again once it is over
    REQUIRE(scene.runtime(0)->glow.level() == 0.0f);
    scene.close();

    // A charge rushes on with no hand on the stick, faster than a run could, and what it runs
    // into is struck.
    member.turbo = 50.0f;
    const std::vector<PartyMember> again{member};
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    REQUIRE(scene.open(device, context, world, again, options));
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const Vec3 from = scene.actor(0)->position();
    const s32 whole = scene.barrels().barrel(barrel).health;
    PlayScene::Inputs tap{};
    tap[0].chargePressed = true;
    scene.update(1.0 / 60.0, tap);
    REQUIRE(scene.animator(0)->shoving());
    for (s32 i = 0; i < 120 && scene.animator(0)->shoving(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const Vec3 to = scene.actor(0)->position();
    REQUIRE(from.z - to.z > 1.5f); // it went at the barrel, which stopped it
    REQUIRE(scene.barrels().barrel(barrel).health < whole);
    scene.close();

    // On a bed of spikes with the guard up, nothing gets through; let down, it does.
    member.turbo = 0.0f;
    const std::vector<PartyMember> third{member};
    options.position = Vec3{-36.2f, 26.5f, -125.3f};
    REQUIRE(scene.open(device, context, world, third, options));
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    PlayScene::Inputs guard{};
    guard[0].turbo = true;
    for (s32 i = 0; i < 600 && !scene.animator(0)->defending(); ++i) {
        scene.update(1.0 / 60.0, guard);
    }
    REQUIRE(scene.animator(0)->defending());
    const s32 guardedFrom = scene.actor(0)->save().health();
    for (s32 i = 0; i < 600; ++i) {
        scene.update(1.0 / 60.0, guard);
    }
    REQUIRE(scene.animator(0)->defending());
    REQUIRE(scene.actor(0)->save().health() == guardedFrom);
    for (s32 i = 0; i < 600 && scene.actor(0)->save().health() == guardedFrom; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.actor(0)->save().health() < guardedFrom);
    scene.close();
}

TEST_CASE("every class has its turbo attacks: they show, strike and are paid for",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("PLAYERS/WAR/SFXBLU/animations.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = root;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{3.0f, 2.0f, -20.0f};
    const PlayScene::Inputs still{};
    PlayScene::Inputs strike{};
    strike[0].turbo = true;
    strike[0].attack = true;
    strike[0].turboAttackPressed = true;
    for (s32 character = 0; character < 16; ++character) {
        for (const bool full : {false, true}) {
            CAPTURE(classCode(character), full);
            CharacterSave save;
            save.name = "AB";
            save.character = character;
            PartyMember member{0, save};
            member.turbo = full ? 100.0f : 60.0f;
            const std::vector<PartyMember> party{member};
            PlayScene scene;
            REQUIRE(scene.open(device, context, world, party, options));
            for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
                scene.update(1.0 / 60.0, still);
            }
            REQUIRE(scene.animator(0) != nullptr); // the class's figure was built
            const f32 before = scene.turboMeter(0)->held();
            scene.update(1.0 / 60.0, strike);
            REQUIRE(scene.animator(0)->action() == (full ? PlayerAnimator::Action::TurboFull
                                                         : PlayerAnimator::Action::TurboStrong));
            usize strikes = 0;
            usize effects = 0;
            for (s32 i = 0; i < 600 && scene.animator(0)->turboing(); ++i) {
                scene.update(1.0 / 60.0, still);
                strikes = std::max(strikes, scene.strikes().count());
                effects = std::max(effects, scene.effects().count());
            }
            REQUIRE_FALSE(scene.animator(0)->turboing());
            REQUIRE(strikes >= 1);
            REQUIRE(effects >= 1); // its own trees, or those of the class it shadows
            const f32 cost = full ? TurboMeter::kFullCost : TurboMeter::kStrongCost;
            REQUIRE(scene.turboMeter(0)->held() < before - cost + 8.0f);
            scene.close();
        }
    }
}

TEST_CASE("the archer's lesser turbo attack lets fly volleys of her own arrows",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("PLAYERS/ARC/SFXBLU/animations.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.character = 3;
    PartyMember member{0, save};
    member.turbo = 60.0f;
    const std::vector<PartyMember> party{member};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{3.0f, 2.0f, -20.0f};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    PlayScene::Inputs strike{};
    strike[0].turbo = true;
    strike[0].attack = true;
    strike[0].turboAttackPressed = true;
    scene.update(1.0 / 60.0, strike);
    usize most = 0;
    bool emptyHanded = false;
    for (s32 i = 0; i < 600 && scene.animator(0)->turboing(); ++i) {
        scene.update(1.0 / 60.0, still);
        most = std::max(most, scene.missiles().count());
        emptyHanded = emptyHanded || !scene.weaponHeld(0);
    }
    REQUIRE(most >= 6);   // two streams, a shot every two and a quarter frames for ten
    REQUIRE(emptyHanded); // the move hides what she holds for most of its length
    REQUIRE(scene.weaponHeld(0));
    scene.close();
}

TEST_CASE("the strong attack is a strong throw; experience is scaled and a kill feeds the meter",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{15.8f, 0.2f, 0.7f}; // open ground by the level's start
    options.yaw = 0.5f;                         // looking away from the barrel beside it
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    PlayScene::Inputs strong{};
    strong[0].strongAttack = true;
    scene.update(1.0 / 60.0, strong);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::StrongThrow);
    f32 largest = 0.0f;
    f32 hardest = 0.0f;
    for (s32 i = 0; i < 400 && scene.animator(0)->strongThrowing(); ++i) {
        scene.update(1.0 / 60.0, still);
        for (usize m = 0; m < scene.missiles().count(); ++m) {
            largest = std::max(largest, scene.missiles().missile(m).scale);
            hardest = std::max(hardest, scene.missiles().missile(m).damage);
        }
    }
    REQUIRE(largest == 2.0f);
    REQUIRE(hardest >= 2.0f * PlayerMissiles::kLeastDamage);
    REQUIRE(scene.turboMeter(0)->held() > 0.0f); // it costs the meter nothing

    // G1 gives 2.85 times what is won; a kill's share of that goes to the meter.
    REQUIRE(world.level()->tuning.experience == Catch::Approx(2.85f));
    const f32 meter = scene.turboMeter(0)->held();
    scene.awardExperience(0, 100);
    REQUIRE(scene.actor(0)->save().experience() == 285);
    REQUIRE(scene.turboMeter(0)->held() == Catch::Approx(meter + 0.025f * 285.0f));
    const f32 fed = scene.turboMeter(0)->held();
    scene.awardExperience(0, 100, false); // won otherwise, it feeds nothing
    REQUIRE(scene.actor(0)->save().experience() == 570);
    REQUIRE(scene.turboMeter(0)->held() == Catch::Approx(fed));
    scene.close();
}

TEST_CASE("a level gained is announced with its number and a hundred health, and a tenth "
          "level defers its costume until the tower",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("text/english.json");
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().health = 400;
    save.progress().experience = levelExperience(9);
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{15.8f, 0.2f, 0.7f};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(experienceLevel(scene.actor(0)->save().experience()) == 9);
    // Enough for the tenth: health and narration are immediate; the costume waits.
    const s32 health = scene.actor(0)->save().health();
    const auto before = scene.figureDirectory(0);
    // (The fields pay experience at their own scale, so the gain may be more than one.)
    scene.awardExperience(0, levelExperience(10) - levelExperience(9) + 1, false);
    scene.update(1.0 / 60.0, still);
    const s32 gained = experienceLevel(scene.actor(0)->save().experience());
    REQUIRE(gained >= 10);
    REQUIRE(scene.actor(0)->save().health() == health + 100);
    REQUIRE(scene.help().showing());
    REQUIRE(scene.help().lines().front() == std::format("LEVEL {}", gained));
    if (before.has_value()) {
        const auto after = scene.figureDirectory(0);
        REQUIRE(after.has_value());
        REQUIRE(after == before);
        REQUIRE(scene.actor(0)->save().progress().promotionPending());
    }
    // Another level, no tier: the message again, the costume kept.
    for (s32 i = 0; i < 700; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const auto tiered = scene.figureDirectory(0);
    scene.awardExperience(0, levelExperience(gained + 1) - levelExperience(gained) + 1, false);
    scene.update(1.0 / 60.0, still);
    const s32 next = experienceLevel(scene.actor(0)->save().experience());
    REQUIRE(next > gained);
    REQUIRE(scene.help().lines().front() == std::format("LEVEL {}", next));
    if (next / 10 == gained / 10) {
        REQUIRE(scene.figureDirectory(0) == tiered);
    }
    scene.close();
}

TEST_CASE("tower returns award permanent familiars with locked controls and persist the result",
          "[game][screens][promotion][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("PLAYERS/WAR/SFXYEL/animations.json");
    test::unpackedOrSkip("PLAYERS/VAL/SFXBLU/animations.json");
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("L1")));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave first;
    first.progress().experience = levelExperience(30);
    first.progress().promotedLevel = 29;
    CharacterSave second;
    second.character = 1;
    second.color = 1;
    second.progress().experience = levelExperience(80);
    second.progress().promotedLevel = 79;
    const std::vector<PartyMember> party{PartyMember{3, first}, PartyMember{1, second}};
    PlayOptions options;
    options.welcome = false;
    options.arrivalWorld = 7;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.promotion().active());
    REQUIRE(scene.familiarTier(3) == 0);
    REQUIRE(scene.familiarTier(1) == 1);
    PlayScene::Inputs moving{};
    moving[3].move = MoveInput{Vec2{1, 0}, 1};
    for (s32 frame = 0; frame < 500 && awaitingEntrance(scene); ++frame) {
        scene.update(1.0 / 60.0, moving);
    }
    REQUIRE_FALSE(awaitingEntrance(scene));
    const Vec3 start = scene.actor(3)->position();
    const auto costume = scene.figureDirectory(s32{3});
    for (s32 frame = 0; frame < 359; ++frame) {
        scene.update(1.0 / 60.0, moving);
    }
    REQUIRE(scene.figureDirectory(s32{3}) == costume);
    REQUIRE(scene.familiarTier(3) == 0);
    scene.update(1.0 / 60.0, moving);
    REQUIRE(scene.familiarTier(3) == 1);
    REQUIRE(scene.actor(3)->save().progress().appearanceLevel() == 30);
    REQUIRE_FALSE(scene.animator(3)->entering());
    for (s32 frame = 0; frame < 1500 && scene.promotion().active(); ++frame) {
        scene.update(1.0 / 60.0, moving);
        REQUIRE(scene.actor(3)->position() == start);
    }
    REQUIRE_FALSE(scene.promotion().active());
    REQUIRE(scene.familiarTier(1) == 2);
    auto carried = scene.party();
    carried[0].save = CharacterSave::fromJson(carried[0].save.toJson());
    carried[1].save = CharacterSave::fromJson(carried[1].save.toJson());
    scene.close();
    REQUIRE(scene.open(device, context, world, carried, options));
    REQUIRE_FALSE(scene.promotion().active());
    REQUIRE(scene.familiarTier(3) == 1);
    REQUIRE(scene.familiarTier(1) == 2);
    scene.close();
}

TEST_CASE("a character strafes with its facing held, rings itself with a potion, and is floored",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().health = 400;
    save.progress().inventory.potions = {1};
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{15.8f, 0.2f, 0.7f};
    options.yaw = 0.5f;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    for (s32 i = 0; i < 400 && scene.animator(0)->action() != PlayerAnimator::Action::Ready; ++i) {
        scene.update(1.0 / 60.0, still); // its entrance plays out
    }
    // Strafing: it moves, in a strafing step, and faces where it did.
    const f32 facing = scene.actor(0)->yaw();
    const Vec3 from = scene.actor(0)->position();
    PlayScene::Inputs sidestep{};
    sidestep[0].strafe = true;
    sidestep[0].move.direction = Vec2{1.0f, 0.0f};
    sidestep[0].move.magnitude = 1.0f;
    bool stepped = false;
    for (s32 i = 0; i < 45; ++i) {
        scene.update(1.0 / 60.0, sidestep);
        stepped = stepped || scene.animator(0)->strafing();
    }
    REQUIRE(stepped);
    REQUIRE(scene.actor(0)->yaw() == facing);
    REQUIRE(glm::distance(scene.actor(0)->position(), from) > 1.0f);
    // With the attack held as well, weapons fly as it goes.
    sidestep[0].attack = true;
    usize flying = 0;
    for (s32 i = 0; i < 90; ++i) {
        scene.update(1.0 / 60.0, sidestep);
        flying = std::max(flying, scene.missiles().count());
    }
    REQUIRE(flying >= 2);
    REQUIRE(scene.actor(0)->yaw() == facing);
    for (s32 i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, still);
    }

    // The shield potion: the potion goes and its ring goes about with the character.
    PlayScene::Inputs ring{};
    ring[0].shieldPotion = true;
    usize rings = 0;
    for (s32 i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, ring);
        rings = std::max(rings, scene.effects().count());
    }
    REQUIRE(scene.actor(0)->save().progress().inventory.potions.empty());
    REQUIRE(rings >= 1);
    PlayScene::Inputs walking{};
    walking[0].move.direction = Vec2{0.0f, 1.0f};
    walking[0].move.magnitude = 1.0f;
    for (s32 i = 0; i < 30; ++i) {
        scene.update(1.0 / 60.0, walking);
    }
    REQUIRE(scene.effects().count() >= 1);
    REQUIRE(glm::distance(scene.effects().effect(0).position, scene.actor(0)->position()) < 0.5f);
    for (s32 i = 0; i < 300; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.effects().count() == 0); // spent

    // A blast from in front of it throws it onto its back; it gets up again.
    const Vec3 at = scene.actor(0)->position();
    const Vec3 ahead = scene.actor(0)->facing();
    scene.blast(at + ahead * 3.0f, 8.0f, 30.0f);
    scene.update(1.0 / 60.0, walking);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::FallBack);
    const Vec3 floored = scene.actor(0)->position();
    for (s32 i = 0; i < 20; ++i) {
        scene.update(1.0 / 60.0, walking);
    }
    // It cannot walk off: only the blast throws it, back the way from the blast.
    const Vec3 thrown = scene.actor(0)->position() - floored;
    REQUIRE(glm::dot(thrown, ahead) < 0.0f);
    REQUIRE(glm::length(thrown) < 2.0f);
    for (s32 i = 0; i < 600 && scene.animator(0)->floored(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.animator(0)->floored());
    scene.blast(scene.actor(0)->position() - ahead * 3.0f, 8.0f, 30.0f);
    scene.update(1.0 / 60.0, still);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::FallForward);
    scene.close();
}

TEST_CASE("the fields' zombies are bred from their generators, chase the party, strike it and are "
          "shot down",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    test::unpackedOrSkip("MONSTERS/ZOM/animations.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().health = 400;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    // Open ground west of a grunt generator of strength two that faces away, east.
    options.position = Vec3{100.0f, 10.2f, -72.5f};
    options.yaw = kPi / 2.0f;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // The fields' generators stand for a party of one, at the level's scales, breeding the
    // fields' own kind: zombies, for the grunts the records name.
    REQUIRE(scene.generators().count() == 47);
    REQUIRE(scene.enemies().kindLoaded(13));
    REQUIRE_FALSE(scene.enemies().kindLoaded(kGruntKind));
    s32 nearest = -1;
    for (usize g = 0; g < scene.generators().count(); ++g) {
        const auto id = static_cast<s32>(g);
        if (glm::distance(scene.generators().positionOf(id), Vec3{111.25f, 10.13f, -72.5f}) <
            1.0f) {
            nearest = id;
        }
    }
    REQUIRE(nearest >= 0);
    REQUIRE(scene.generators().tierOf(nearest) == 2);
    REQUIRE(scene.generators().mostOf(nearest) == 3); // five, at the level's three quarters
    REQUIRE(scene.generators().healthOf(nearest) == Approx(15.0f));
    // Grunts are bred for the party near them, come round to it and strike it.
    const s32 health = scene.actor(0)->save().health();
    usize most = 0;
    s32 bitten = -1;
    for (s32 i = 0; i < 900; ++i) {
        scene.update(1.0 / 60.0, still);
        most = std::max(most, scene.enemies().count());
        if (bitten < 0 && scene.actor(0)->save().health() < health) {
            bitten = i;
        }
    }
    REQUIRE(scene.generators().bredOf(nearest) >= 1);
    REQUIRE(most >= 3);
    REQUIRE(most <= 13);
    REQUIRE(bitten > 0);
    REQUIRE(scene.actor(0)->save().health() < health);
    // Weapons thrown into them hurt and kill them, worth experience to the thrower; what
    // one takes it takes from what it deals.
    PlayScene::Inputs throwing{};
    throwing[0].attack = true;
    const s32 experience = scene.actor(0)->save().experience();
    usize fewest = most;
    for (s32 i = 0; i < 900; ++i) {
        scene.update(1.0 / 60.0, throwing);
        fewest = std::min(fewest, scene.enemies().count());
    }
    REQUIRE(scene.actor(0)->save().experience() > experience);
    REQUIRE(fewest < most);
    // A generator is struck through its brood: shot enough it crumbles, then is gone.
    const usize before = scene.effects().count();
    scene.generators().strike(nearest, 40.0f, 0);
    REQUIRE(scene.generators().stateOf(nearest) < 3);
    for (s32 i = 0; i < 3000 && scene.generators().standing(nearest); ++i) {
        scene.update(1.0 / 60.0, throwing);
    }
    REQUIRE_FALSE(scene.generators().standing(nearest));
    (void)before;
    scene.close();
}

TEST_CASE("Chimera scenario walks up the stairs before lowering the occupied platform",
          "[game][screens][camera][chimera-approach][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELA5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/level-a5-chimera.json");
    REQUIRE_FALSE(scenario.tower.position.has_value());
    const s32 rate = GENERATE(30, 60);
    CAPTURE(rate);
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto arena = levels.byName(scenario.level);
    REQUIRE(arena.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *arena));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, scenario.partyMembers(), scenario.tower));
    const auto* start = world.startPoint(0);
    const auto* boss = world.layout().findLocator(LocatorKind::Boss);
    REQUIRE(start != nullptr);
    REQUIRE(boss != nullptr);
    REQUIRE(scene.actor(0) != nullptr);
    CHECK(scene.actor(0)->position().x == Approx(start->position.x));
    CHECK(scene.actor(0)->position().z == Approx(start->position.z));
    const auto entranceFloor = world.collision().floorAt(start->position, 4, 10);
    REQUIRE(entranceFloor.has_value());
    CHECK(scene.actor(0)->position().y == Approx(entranceFloor->y));
    REQUIRE(scene.bosses().position() != nullptr);
    const Vec3 initialBoss = *scene.bosses().position();
    CHECK(glm::distance(initialBoss, boss->position) < 1.01f);
    CHECK(scene.bosses().facing() == Approx(boss->rotation.y));
    constexpr usize kElevator = 25;
    REQUIRE(world.layout().objects()[kElevator].name == "A5ELEVATOR");
    const f32 initialLift = world.scene().worldTransform(kElevator)[3].y;
    REQUIRE(scene.startCamera().active());
    REQUIRE(world.entranceCamera().has_value());
    CHECK(scene.viewCamera().position == world.entranceCamera()->position);
    CHECK(scene.bossCamera().pitch() == Approx(0.645772f).margin(0.001f));
    CHECK(scene.bossCamera().distance() == Approx(30.0f));
    WorldCamera previous = scene.viewCamera();
    for (s32 frame = 0; frame < rate * 30 && awaitingEntrance(scene); ++frame) {
        previous = scene.viewCamera();
        scene.update(1.0 / rate, {});
    }
    REQUIRE_FALSE(awaitingEntrance(scene));
    CHECK(glm::distance(previous.position, scene.viewCamera().position) < 0.5f);
    CHECK_FALSE(scene.bosses().view().awake);
    // Waiting at the bottom must not count as stepping on the pressure region.
    for (s32 frame = 0; frame < rate * 2; ++frame) {
        scene.update(1.0 / rate, {});
    }
    CHECK_FALSE(world.triggers().opened(kElevator));
    CHECK(world.scene().worldTransform(kElevator)[3].y == Approx(initialLift));
    CHECK(*scene.bosses().position() == initialBoss);
    PlayScene::Inputs walking{};
    walking[0].move = {{0, 1}, 1};
    bool halfway = false;
    bool reachedTop = false;
    for (s32 frame = 0; frame < rate * 25; ++frame) {
        if (scene.actor(0)->position().z <= 10) {
            walking[0].move = {};
        }
        scene.update(1.0 / rate, walking);
        reachedTop |= scene.actor(0)->position().y > -0.5f;
        const f32 descent = initialLift - world.scene().worldTransform(kElevator)[3].y;
        if (!halfway && descent > 10 && descent < 20) {
            halfway = true;
            CHECK(scene.bosses().position()->y == Approx(initialBoss.y - descent).margin(0.2f));
            CHECK(reachedTop);
        }
    }
    CAPTURE(scene.actor(0)->position().y, scene.actor(0)->position().z);
    CHECK(reachedTop);
    CHECK(halfway);
    CHECK(world.triggers().opened(kElevator));
    CHECK(world.scene().worldTransform(kElevator)[3].y == Approx(initialLift - 23));
    CHECK(scene.bosses().position()->y == Approx(initialBoss.y - 23).margin(0.2f));
    CHECK(scene.bosses().position()->x == Approx(initialBoss.x));
    // Its authored STEP moves may advance it within the TYPE's twelve-unit home
    // radius after waking; the boss is not pinned to its initial z throughout combat.
    CHECK(std::abs(scene.bosses().position()->z - initialBoss.z) <= 12.01f);
    CHECK(scene.bosses().view().awake);
    CHECK(scene.viewCamera().position.z > scene.actor(0)->position().z);
}

TEST_CASE("Garm's entrance rides to the boss camera without a floor detour or handoff cut",
          "[game][screens][camera][garm-arrival][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELH4/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GARM/animations.json");
    test::unpackedOrSkip("critter/GARM.json");
    const s32 rate = GENERATE(30, 60);
    CAPTURE(rate);
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto chamber = levels.byName("H4");
    REQUIRE(chamber.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *chamber));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.progress().experience = levelExperience(99);
    save.progress().health = 10000;
    const std::array party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.bossCameraOn());
    REQUIRE(scene.startCamera().active());
    REQUIRE(scene.startCamera().mode() == StartCamera::Mode::Legacy);
    // Both boss entrance solvers look at the party before it wakes. The ordinary level
    // camera is not an intermediate destination on the way to the boss camera.
    const Vec3 attention = scene.bossCamera().attention();
    REQUIRE(glm::distance(scene.startCamera().attention(), attention) < 0.01f);
    WorldCamera before = scene.viewCamera();
    for (s32 frame = 0; frame < rate * 30 && scene.spawning(); ++frame) {
        before = scene.viewCamera();
        scene.update(1.0 / rate, {});
        if (scene.startCamera().active()) {
            REQUIRE(glm::distance(scene.startCamera().attention(), attention) < 0.01f);
        }
    }
    REQUIRE_FALSE(scene.spawning());
    const WorldCamera after = scene.viewCamera();
    CHECK(glm::distance(before.position, after.position) < StartCamera::kArrival + 0.01f);
    CHECK(glm::distance(before.forward(), after.forward()) < 0.02f);
    CHECK(after.position == scene.bossCamera().camera().position);
    // The next update wakes Garm and follows the same camera, not a second abrupt cut.
    scene.update(1.0 / rate, {});
    REQUIRE(scene.bosses().view().awake);
    CHECK(glm::distance(after.forward(), scene.viewCamera().forward()) < 0.05f);
}

TEST_CASE("Lich entrance uses its authored camera and seals the walkway before waking",
          "[screens][lich-entrance][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG5/world.json");
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    const auto scenario =
        Scenario::load(test::dataDirectory().parent_path() / "tests/scenarios/level-g5-lich.json");
    const s32 rate = GENERATE(30, 60);
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName("G5");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, scenario.partyMembers(), scenario.tower));
    const auto* start = world.startPoint(0);
    REQUIRE(start);
    CHECK(scene.actor(0)->position().x == Approx(start->position.x));
    CHECK(scene.actor(0)->position().z == Approx(start->position.z));
    REQUIRE(scene.startCamera().active());
    REQUIRE(world.entranceCamera());
    CHECK(scene.viewCamera().position == world.entranceCamera()->position);
    // The approach marker nearest the start in XZ is (0.5, 31, 65), pitched 25 degrees.
    CHECK(scene.bossCamera().pitch() == Approx(0.4363321f).margin(0.001f));
    CHECK_FALSE(scene.bosses().view().awake);
    constexpr usize kBlade = 337;
    REQUIRE(world.layout().objects()[kBlade].name == "G5BLADE");
    const f32 bladeStart = world.scene().worldTransform(kBlade)[3].y;
    WorldCamera before = scene.viewCamera();
    for (s32 frame = 0; frame < rate * 30 && awaitingEntrance(scene); ++frame) {
        before = scene.viewCamera();
        scene.update(1.0 / rate, {});
    }
    REQUIRE_FALSE(awaitingEntrance(scene));
    CHECK(glm::distance(before.position, scene.viewCamera().position) < 0.5f);
    CHECK(glm::distance(before.forward(), scene.viewCamera().forward()) < 0.05f);
    CHECK_FALSE(scene.bosses().view().awake);
    // Walk away from the burial, toward the blade behind the authored start.
    PlayScene::Inputs walk{};
    walk[0].move = {{0, -1}, 1};
    for (s32 frame = 0; frame < rate * 6; ++frame) {
        scene.update(1.0 / rate, walk);
    }
    CAPTURE(scene.actor(0)->position().z, world.scene().worldTransform(kBlade)[3].y);
    CHECK(world.triggers().opened(kBlade));
    CHECK(world.scene().worldTransform(kBlade)[3].y < bladeStart - 10);
    CHECK(scene.actor(0)->position().z < 63);
    CHECK_FALSE(scene.bosses().view().awake);
    // Reverse direction: the boss wakes only on approaching the burial.
    walk[0].move = {{0, 1}, 1};
    for (s32 frame = 0; frame < rate * 15 && !scene.bosses().view().awake; ++frame) {
        before = scene.viewCamera();
        scene.update(1.0 / rate, walk);
    }
    CHECK(scene.bosses().view().awake);
    CHECK(glm::distance(before.forward(), scene.viewCamera().forward()) < 0.1f);
}

TEST_CASE("in the town's crypt the lich rises for the party, its meter over the screen, and "
          "the book of protection brought to it is thrown and takes its quarter",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG5/world.json");
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::unpackedOrSkip("critter/LICH.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto crypt = levels.byName("G5");
    REQUIRE(crypt.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *crypt));
    REQUIRE(world.level() != nullptr);
    REQUIRE(world.level()->bossType == 41);
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().relics.addLegend(7); // the book of protection, the town's
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    // Open ground thirty off the boss mark, within the lich's threshold of thirty-nine.
    options.position = Vec3{0.0f, 0.2f, 30.0f};
    options.yaw = kPi;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.bosses().present());
    REQUIRE(scene.bossView().has_value());
    REQUIRE(scene.bossView()->name == "LICH");
    REQUIRE(scene.bosses().legend().stage() == LegendRite::Stage::Carried);
    REQUIRE(scene.bossMeter().bound());
    const f32 approachPitch = scene.bossCamera().pitch();
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // The boss stands at its mark; the meter shows it whole; the fight's own camera looks
    // from behind the party toward the boss. The approach pitch eases into the fight,
    // rather than snapping to the combat record's shallow angle on the wake frame.
    REQUIRE(glm::distance(*scene.bosses().position(), Vec3{0.0f, 0.0f, -0.5f}) < 2.0f);
    REQUIRE(world.level()->bossCamera.has_value());
    REQUIRE(scene.bossCameraOn());
    REQUIRE(scene.viewCamera().position.z > scene.actor(0)->position().z);
    REQUIRE(scene.viewCamera().pitch < approachPitch);
    REQUIRE(scene.bossCamera().margin() >= 0.0f);
    REQUIRE(scene.bossMeter().showing());
    REQUIRE(scene.bossMeter().count() == 1);
    REQUIRE(scene.bossMeter().meter(0).shown() == scene.bossView()->maxHealth);
    REQUIRE(scene.bossMeter().meter(0).fillWidths()[0] == BossMeter::kPieceWidth);
    // It wakes for the party; the book is spent as it is held up, and thrown it takes a
    // quarter of the lich, which the meter follows down.
    const f32 whole = scene.bossView()->maxHealth;
    s32 waited = 0;
    bool entrance = false; // rising, it plays its entrance's effect from its own archive
    bool held = false;     // the book glows over the bearer's head while it is held up
    const auto find = [&scene](std::string_view tree) -> const EffectTrees::Effect* {
        for (usize e = 0; e < scene.effects().count(); ++e) {
            if (scene.effects().effect(e).name == tree) {
                return &scene.effects().effect(e);
            }
        }
        return nullptr;
    };
    const auto showing = [&find](std::string_view tree) { return find(tree) != nullptr; };
    while (!scene.bosses().legend().thrown() && waited < 3000) {
        scene.update(1.0 / 60.0, still);
        entrance = entrance || showing("GENFX");
        if (const EffectTrees::Effect* book = find(LegendShow::kHeldTree); book != nullptr) {
            held = true;
            REQUIRE(book->position.y ==
                    Approx(scene.actor(0)->position().y + LegendShow::kHeldLift).margin(0.5f));
        }
        ++waited;
    }
    REQUIRE(scene.bosses().view().awake);
    REQUIRE(scene.bosses().legend().thrown());
    REQUIRE(scene.viewCamera().pitch <= world.level()->bossCamera->maxPitch + 0.01f);
    REQUIRE(entrance);
    REQUIRE(held);
    // The level has gone dark for the rite, as for a great move, but the lich stands in
    // the level's own light: its figure is drawn brighter than the dimmed light would.
    REQUIRE(scene.bosses().legend().darkens());
    REQUIRE(scene.dimmer().offset() <= LegendRite::kDarkening + 0.1f);
    // The bearer shines through the rite (player.c 2508).
    REQUIRE(scene.runtime(scene.bosses().legend().player())->glow.level() == BodyGlow::kBearer);
    {
        const auto brightness = [](const std::vector<test::RecordedDraw>& draws,
                                   const std::set<const Texture*>& of) {
            f32 sum = 0.0f;
            usize count = 0;
            for (const test::RecordedDraw& draw : draws) {
                if (!of.contains(draw.texture)) {
                    continue;
                }
                for (const ImmediateVertex& v : draw.vertices) {
                    sum += static_cast<f32>(v.color.r + v.color.g + v.color.b);
                    ++count;
                }
            }
            return count > 0 ? sum / static_cast<f32>(count) : -1.0f;
        };
        test::FakeRenderDevice dimmed;
        scene.bosses().draw(dimmed, Mat4{1.0f}, world.lighting());
        std::set<const Texture*> bossTextures;
        for (const test::RecordedDraw& draw : dimmed.draws) {
            bossTextures.insert(draw.texture);
        }
        REQUIRE_FALSE(bossTextures.empty());
        device.draws.clear();
        scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
        REQUIRE(brightness(device.draws, bossTextures) >
                brightness(dimmed.draws, bossTextures) * 1.5f);
    }
    REQUIRE_FALSE(scene.actor(0)->save().progress().relics.hasLegend(7));
    REQUIRE(scene.bossView()->health == Approx(whole - (0.25f * whole - 1.0f)));
    const f32 struck = scene.bossView()->health;
    scene.update(1.0 / 60.0, still);
    REQUIRE(scene.bossMeter().meter(0).shown() > struck); // three a tick, not at once
    // The bearer makes the gesture of a potion used, at whose release the book leaves the
    // hand and is set burning on the lich.
    bool gestured = false;
    bool landed = false;
    for (s32 i = 0; i < 600 && !landed; ++i) {
        scene.update(1.0 / 60.0, still);
        const PlayerAnimator* body = scene.animator(0);
        REQUIRE(body != nullptr);
        gestured = gestured || body->action() == PlayerAnimator::Action::UsePotion;
        landed = showing(LegendShow::kProjectileTree);
    }
    REQUIRE(gestured);
    REQUIRE(landed);
    REQUIRE_FALSE(showing(LegendShow::kHeldTree));
    REQUIRE(glm::distance(find(LegendShow::kProjectileTree)->position, *scene.bosses().position()) <
            1.0f);
    REQUIRE(scene.actor(0)->save().progress().inventory.potions.empty()); // none spent
    // Burnt through, the book gives way to its fire for the lich's five seconds.
    for (s32 i = 0; i < 60 && !showing(LegendShow::kBurstTree); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(showing(LegendShow::kBurstTree));
    for (s32 i = 0; i < 600 && scene.bossMeter().meter(0).shown() > struck; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.bossMeter().meter(0).shown() <= scene.bossView()->health);
    REQUIRE(scene.bossMeter().meter(0).fillWidths()[1] < BossMeter::kPieceWidth - 53);
    // Roared, the lich fights on in the light again.
    for (s32 i = 0; i < 600 && scene.bosses().legend().darkens(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.bosses().legend().darkens());
    for (s32 i = 0; i < 600 && scene.dimmer().offset() < -0.05f; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.dimmer().offset() >= -0.05f);
    // Slain, the lich leaves the town's shard to everyone and its key where it fell; the
    // wizard comes five seconds on, names it beaten, counts the realm's runestones (none
    // found), and sees the party off to the tower.
    EnemyHit slay;
    slay.damage = 100000.0f;
    slay.player = 0;
    const Vec3 shardSpot = *scene.bosses().position() + scene.bosses().rewardOffset();
    scene.bosses().hurt(slay);
    scene.update(1.0 / 60.0, still);
    REQUIRE_FALSE(scene.bossView()->alive);
    REQUIRE_FALSE(scene.victory().running()); // Death animation owns the coin spew first.
    REQUIRE_FALSE(scene.bossMeter().showing());
    // At its death's 95th frame the lich throws the town's coins for the party (four
    // bronze and a silver for one) all round it, up steeply, which sail out and come down.
    REQUIRE_FALSE(world.goldLeft());
    const usize itemsBefore = world.placedItems().size();
    for (s32 i = 0; i < 400 && !world.goldLeft(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(world.goldLeft());
    REQUIRE(world.placedItems().size() == itemsBefore + 5);
    usize bronze = 0;
    usize aloft = 0;
    for (usize i = itemsBefore; i < world.placedItems().size(); ++i) {
        const PlacedItems::Item& coin = world.placedItems().item(i);
        bronze += coin.name == "COIN_BRONZE" ? 1 : 0;
        aloft += coin.thrown ? 1 : 0;
        REQUIRE_FALSE(coin.takeable());
    }
    REQUIRE(bronze == 4);
    REQUIRE(aloft == 5);
    for (s32 i = 0; i < 1200 && !scene.victory().running(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.victory().running());
    REQUIRE(scene.victory().stage() == BossVictory::Stage::Waiting);
    REQUIRE(LevelRef::orderOf(7) == 1);
    REQUIRE(scene.actor(0)->save().progress().relics.hasShard(1));
    bool key = false;
    for (usize e = 0; e < scene.effects().count(); ++e) {
        key = key || scene.effects().effect(e).name == "BOSSKEY";
        if (scene.effects().effect(e).name == "BOSSKEY") {
            REQUIRE(scene.effects().effect(e).position == shardSpot);
        }
    }
    REQUIRE(key);
    for (s32 i = 0; i < 400 && scene.victory().stage() != BossVictory::Stage::Defeat; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.victory().stage() == BossVictory::Stage::Defeat);
    REQUIRE(scene.victory().caption()->message == "LICH_SPEECH");
    {
        // The wizard adds onto the frame without writing depth, like the tower's: the
        // level's translucent scenery must be down before any of him.
        const auto wizard = world.items().trees.find("WIZARD");
        REQUIRE(wizard.has_value());
        std::vector<const Texture*> his;
        for (const TreeNodeInfo& node : world.items().trees.tree(*wizard).nodes) {
            if (const auto model = world.items().models.find(node.object)) {
                for (const auto& part : world.items().models.mesh(*model).parts) {
                    his.push_back(&world.items().textures.texture(device, part.texture));
                }
            }
        }
        REQUIRE_FALSE(his.empty());
        test::FakeRenderDevice deferred;
        world.scene().drawDeferred(deferred, Mat4{1}, CameraFrame::of(scene.viewCamera()));
        std::vector<const Texture*> scenery;
        for (const auto& draw : deferred.draws) {
            if (draw.blend() != BlendMode::Additive &&
                std::ranges::find(his, draw.texture) == his.end()) {
                scenery.push_back(draw.texture);
            }
        }
        device.draws.clear();
        scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
        std::optional<usize> firstWizard;
        std::optional<usize> lastScenery;
        for (usize i = 0; i < device.draws.size(); ++i) {
            const Texture* texture = device.draws[i].texture;
            if (!firstWizard.has_value() && std::ranges::find(his, texture) != his.end()) {
                firstWizard = i;
            }
            if (std::ranges::find(scenery, texture) != scenery.end()) {
                lastScenery = i;
            }
        }
        REQUIRE(firstWizard.has_value());
        if (lastScenery.has_value()) {
            REQUIRE(*lastScenery < *firstWizard);
        }
    }
    REQUIRE(scene.bossCameraOn()); // the camera stays the fight's, on the wizard
    // Movement follows the camera the player actually sees after the focus changes.
    const Vec3 beforeStep = scene.actor(0)->position();
    const f32 yaw = scene.bossCamera().yaw();
    PlayScene::Inputs walk{};
    walk[0].move = MoveInput{Vec2{1, 0}, 1};
    scene.update(1.0 / 60.0, walk);
    const Vec3 stepped = scene.actor(0)->position() - beforeStep;
    REQUIRE(glm::dot(stepped, Vec3{std::cos(yaw), 0, -std::sin(yaw)}) > 0);
    // The coins come down within a few seconds and can be taken (those that flew off the
    // level's edge are lost); left lying, they keep the wizard waiting ten seconds after
    // his lines.
    const auto stillFlying = [&world, itemsBefore] {
        for (usize i = itemsBefore; i < world.placedItems().size(); ++i) {
            if (world.placedItems().item(i).thrown) {
                return true;
            }
        }
        return false;
    };
    s32 leaving = 0;
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 i = 0; i < 600 && stillFlying() && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
        leaving += scene.victory().stage() == BossVictory::Stage::Leaving ? 1 : 0;
    }
    REQUIRE_FALSE(stillFlying());
    usize down = 0;
    for (usize i = itemsBefore; i < world.placedItems().size(); ++i) {
        const PlacedItems::Item& coin = world.placedItems().item(i);
        REQUIRE(coin.takeable() == coin.visible);
        down += coin.visible ? 1 : 0;
    }
    REQUIRE(down >= 1);
    REQUIRE(world.goldLeft());
    for (s32 i = 0; i < 6000 && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
        leaving += scene.victory().stage() == BossVictory::Stage::Leaving ? 1 : 0;
    }
    REQUIRE(leaving > BossVictory::kExitLongTicks - BossVictory::kExitSparkleTicks - 2);
    REQUIRE(scene.victory().runeQuality() == 0);
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(scene.destination().isTower());
    scene.close();
    REQUIRE_FALSE(scene.bossMeter().bound());
}

TEST_CASE("in the mountain's lair the ice axe is held in the hand, thrown with the strong "
          "throw, flies at the dragon and freezes it, and its death spews silver",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELB6/world.json");
    test::unpackedOrSkip("ITEMS/LEVELB6/animations.json");
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
    test::unpackedOrSkip("critter/DRAGON.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto lair = levels.byName("B6");
    REQUIRE(lair.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *lair));
    REQUIRE(world.level() != nullptr);
    REQUIRE(world.level()->bossType == 34);
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().relics.addLegend(2); // the ice axe, the mountain's
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    // Open ground before the dragon's mark at (-3, 30, -18).
    options.position = Vec3{-5.0f, 30.0f, 8.0f};
    options.yaw = kPi;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.bosses().present());
    REQUIRE(scene.bossView()->name == "DRAGON");
    REQUIRE(scene.safeRocks().size() == 6);
    REQUIRE(scene.safeRocks().obstacles().size() == 6);
    REQUIRE(scene.bosses().height() == Approx(4.5f)); // root 18.5, body origin -14
    REQUIRE(scene.bosses().legend().stage() == LegendRite::Stage::Carried);
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const auto find = [&scene](std::string_view tree) -> const EffectTrees::Effect* {
        for (usize e = 0; e < scene.effects().count(); ++e) {
            if (scene.effects().effect(e).name == tree) {
                return &scene.effects().effect(e);
            }
        }
        return nullptr;
    };
    // The axe glows in the bearer's hand, by the body rather than high over the head.
    bool held = false;
    bool charged = false;
    s32 waited = 0;
    while (!scene.bosses().legend().thrown() && waited < 3000) {
        scene.update(1.0 / 60.0, still);
        charged =
            charged || (find(LegendShow::kAuraTree) != nullptr &&
                        find(LegendShow::chargeTree(scene.actor(0)->save().color)) != nullptr);
        if (const EffectTrees::Effect* axe = find(LegendShow::kHeldTree); axe != nullptr) {
            held = true;
            REQUIRE(axe->unlit);
            const f32 over = axe->position.y - scene.actor(0)->position().y;
            REQUIRE(over < LegendShow::kHeldLift - 1.0f);
            REQUIRE(glm::distance(axe->position, scene.actor(0)->position()) < 8.0f);
        }
        ++waited;
    }
    REQUIRE(scene.bosses().legend().thrown());
    REQUIRE(held);
    REQUIRE(charged);
    REQUIRE(scene.bosses().legend().darkens());
    const f32 whole = scene.bossView()->maxHealth;
    REQUIRE(scene.bossView()->health == Approx(whole));
    REQUIRE_FALSE(scene.bosses().frozen());
    // The strong throw's gesture lets it fly toward the dragon without the weapon going,
    // and it lands within its flight's time.
    bool gestured = false;
    bool flying = false;
    bool trailVisible = false;
    f32 nearest = 1000.0f;
    for (s32 i = 0; i < 900 && (!flying || find(LegendShow::kProjectileTree) != nullptr); ++i) {
        scene.update(1.0 / 60.0, still);
        gestured = gestured || scene.animator(0)->action() == PlayerAnimator::Action::StrongThrow;
        if (const EffectTrees::Effect* axe = find(LegendShow::kProjectileTree); axe != nullptr) {
            flying = true;
            REQUIRE(axe->unlit);
            REQUIRE(axe->trails.size() == 1);
            trailVisible = trailVisible || axe->trails.particleCount() > 0;
            REQUIRE(glm::length(axe->velocity) == Approx(LegendShow::kSpeed));
            nearest = std::min(nearest, glm::distance(axe->position, *scene.bosses().position()));
        }
    }
    REQUIRE(gestured);
    REQUIRE(flying);
    REQUIRE(trailVisible);
    REQUIRE(find(LegendShow::kProjectileTree) == nullptr);
    REQUIRE(nearest < 12.0f);
    REQUIRE(scene.missiles().count() == 0);
    REQUIRE_FALSE(find(LegendShow::kHeldTree));
    REQUIRE(scene.bossView()->health == Approx(whole - (0.1f * whole - 1.0f)));
    REQUIRE(scene.bosses().frozen());
    REQUIRE_FALSE(scene.bosses().legend().darkens());
    const auto iceSlot = world.items().textures.find("SEETHROUGH");
    REQUIRE(iceSlot.has_value());
    const Texture& ice = world.items().textures.texture(device, *iceSlot);
    device.draws.clear();
    scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
    REQUIRE(std::ranges::any_of(
        device.draws, [&ice](const auto& draw) { return draw.state.maskedTexture == &ice; }));
    device.draws.clear();
    scene.bosses().draw(device, Mat4{1.0f}, world.lighting(), &ice);
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        REQUIRE(draw.texture != &ice);
        REQUIRE(draw.state.maskedTexture == &ice);
        if (draw.state.alphaTest == 0.0f) {
            REQUIRE(draw.state.blend == BlendMode::Opaque);
        }
    }
    const f32 afterImpact = scene.bossView()->health;
    scene.bosses().landLegend();
    REQUIRE(scene.bossView()->health == afterImpact); // duplicate presentation callback is harmless
    // Lighting recovers while the freeze is still active, not after its twenty seconds.
    for (s32 i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.bosses().frozen());
    REQUIRE_FALSE(scene.bosses().legend().darkens());
    REQUIRE(world.ambientOffset() == Approx(0.0f).margin(0.001f));
    // Thawed, the dragon fights on; slain, it throws the mountain's five silver coins.
    for (s32 i = 0; i < 1500 && scene.bosses().frozen(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.bosses().frozen());
    EnemyHit slay;
    slay.damage = 100000.0f;
    slay.player = 0;
    scene.bosses().hurt(slay);
    const usize itemsBefore = world.placedItems().size();
    for (s32 i = 0; i < 600 && !world.goldLeft(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(world.goldLeft());
    REQUIRE(world.placedItems().size() == itemsBefore + 5);
    for (usize i = itemsBefore; i < world.placedItems().size(); ++i) {
        REQUIRE(world.placedItems().item(i).name == "COIN_SILVER");
        REQUIRE(world.placedItems().item(i).value == 1000);
    }
    // The death animation owns the spew; the victory begins once the body is gone.
    for (s32 i = 0; i < 1200 && !scene.victory().running(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.victory().running());
    scene.close();
}

TEST_CASE("a character hurt cries out by the original's rules: at once for a burn or a heavy "
          "blow, once lesser blows add up, and is named as its health runs low",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    test::unpackedOrSkip("audio/WAR/sounds.json");
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.config = &config;
    context.sounds = &sounds;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    save.progress().health = 900;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f};
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    for (s32 i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, still); // the level's own sounds settle
    }
    std::vector<f32> stereo(static_cast<usize>(2 * 4800), 0.0f);
    const auto live = [&] {
        mixer.mix(stereo); // stopped voices drain and are let go of
        sounds.update();
        return sounds.voiceCount();
    };
    sounds.stopAll();
    // A light blow is felt (the hit sounds) but not cried over; enough of them are: once
    // thirty of harm has been taken, counted before the health is rounded (pain_accum), so
    // what the level and the armour leave of each blow decides which one it is.
    const s32 whole = scene.actor(0)->save().health();
    scene.harm(0, 5.0f, HurtKind::Blow);
    REQUIRE(live() == 1);
    sounds.stopAll();
    REQUIRE(live() == 0);
    const s32 perBlow = whole - scene.actor(0)->save().health();
    REQUIRE(perBlow > 0);
    s32 cries = 0;
    s32 blows = 1;
    while (cries == 0 && blows < 20) {
        scene.harm(0, 5.0f, HurtKind::Blow);
        ++blows;
        cries = static_cast<s32>(live()); // the hit's own sound waits its half second
    }
    REQUIRE(cries == 1);
    const s32 lost = whole - scene.actor(0)->save().health();
    CHECK(lost >= 30 - perBlow);
    CHECK(lost <= 30 + 2 * perBlow);
    sounds.stopAll();
    // A heavy blow, or a burn, is cried over at once.
    scene.harm(0, 61.0f, HurtKind::Blow);
    REQUIRE(live() == 1);
    sounds.stopAll();
    // (A burn the armour takes whole is not felt at all.)
    scene.harm(0, 1.5f, HurtKind::Burn);
    REQUIRE(live() == 0);
    scene.harm(0, 5.0f, HurtKind::Burn);
    REQUIRE(live() == 1);
    sounds.stopAll();
    // Down past a hundred and fifty the narrator names the character instead.
    const s32 health = scene.actor(0)->save().health();
    REQUIRE(health > 150);
    scene.harm(0, static_cast<f32>(health - 140), HurtKind::Burn);
    REQUIRE(scene.actor(0)->save().health() <= 150); // less what the armour takes
    REQUIRE(scene.actor(0)->save().health() > 50);
    REQUIRE(live() >= 1); // the name, the line queued after it
    sounds.stopAll();
    scene.harm(0, 100.0f, HurtKind::Burn);
    REQUIRE(scene.actor(0)->save().health() <= 50);
    REQUIRE(scene.actor(0)->save().health() > 0);
    REQUIRE(live() >= 1);
    scene.close();
}

TEST_CASE("potions burst about the character or where they land, and powerups show",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    Inventory& carried = save.progress().inventory;
    carried.potions = {1, 4};
    carried.addPowerup(powerup::kWeapon, powerup::kThreeWayShot, 0.0f, 30.0f);
    carried.addPowerup(powerup::kSpeed, 0, 3.0f, 30.0f);
    carried.addPowerup(powerup::kSpecial, powerup::kGrowth, 0.0f, 30.0f);
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const Inventory& now = scene.actor(0)->save().progress().inventory;
    // Worn, the speed powerup quickens the character and growth enlarges them.
    scene.update(1.0 / 60.0, still);
    CharacterSave plain;
    plain.name = "CD";
    PlayerActor bare;
    bare.spawn(0, plain, nullptr, Vec3{0.0f}, 0.0f);
    REQUIRE(PlayerFigure::bodyScale(scene.actor(0)->save(), PowerupEffects::of(now)) ==
            PowerupEffects::kGrowthScale);
    REQUIRE(PlayerFigure::bodyScale(plain, PowerupEffects{}) == 1.0f);

    // The green potion, taken last, is used first: its acid bursts about the character.
    PlayScene::Inputs use{};
    use[0].usePotion = true;
    for (s32 i = 0; i < 60 && scene.effects().count() == 0; ++i) {
        scene.update(1.0 / 60.0, use);
    }
    REQUIRE(scene.effects().count() == 1);
    REQUIRE(scene.effects().effect(0).name == "MP_ACID");
    // A warrior's little magic makes it a small one.
    REQUIRE(scene.effects().effect(0).scale > 0.25f);
    REQUIRE(scene.effects().effect(0).scale < 0.5f);
    REQUIRE(now.potions == std::vector<s32>{1});
    // Held on, no second potion goes; released and thrown, the red one flies and bursts.
    for (s32 i = 0; i < 200; ++i) {
        scene.update(1.0 / 60.0, use);
    }
    REQUIRE(now.potions.size() == 1);
    REQUIRE(scene.effects().count() == 0);
    PlayScene::Inputs toss{};
    toss[0].throwPotion = true;
    scene.update(1.0 / 60.0, still);
    for (s32 i = 0; i < 60 && scene.missiles().count() == 0; ++i) {
        scene.update(1.0 / 60.0, toss);
    }
    REQUIRE(scene.missiles().count() == 1);
    REQUIRE(scene.missiles().missile(0).potion == 1);
    REQUIRE(now.potions.empty());
    for (s32 i = 0; i < 240 && scene.effects().count() == 0; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.missiles().count() == 0);
    REQUIRE(scene.effects().count() == 1);
    REQUIRE(scene.effects().effect(0).name == "MP_FIRE");
    // With no potion left the buttons do nothing.
    for (s32 i = 0; i < 30; ++i) {
        scene.update(1.0 / 60.0, use);
    }
    REQUIRE_FALSE(scene.animator(0)->conjuring());

    // The three way shot throws three axes at once; taken off in the selector, one.
    PlayScene::Inputs attack{};
    attack[0].attack = true;
    for (s32 i = 0; i < 60 && scene.missiles().count() == 0; ++i) {
        scene.update(1.0 / 60.0, attack);
    }
    REQUIRE(scene.missiles().count() == 3);
    for (s32 i = 0; i < 300; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    PlayScene::Inputs open{};
    open[0].selector.up = true;
    scene.update(1.0 / 60.0, open);
    for (s32 i = 0; i < 40 && !scene.selector(0).showing(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.selector(0).showing());
    PlayScene::Inputs left{};
    left[0].selector.left = true;
    for (s32 i = 0; i < 4 && now.powerups[static_cast<usize>(scene.selector(0).selection())].kind !=
                                 powerup::kWeapon;
         ++i) {
        scene.update(1.0 / 60.0, left);
    }
    scene.update(1.0 / 60.0, open); // up again: off it comes
    REQUIRE(PowerupEffects::of(now).shots() == 1);
    for (s32 i = 0; i < 60 && scene.missiles().count() == 0; ++i) {
        scene.update(1.0 / 60.0, attack);
    }
    REQUIRE(scene.missiles().count() == 1);
    scene.close();
}

TEST_CASE("holding the attack throws the character's weapon again and again",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.config = &config;
    context.sounds = &sounds;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < 400 && awaitingEntrance(scene); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE(scene.missiles().count() == 0);
    REQUIRE(scene.weaponHeld(0));
    const Vec3 stood = scene.actor(0)->position();
    // Attacking with the stick pushed: the body throws where it stands, and the axe leaves
    // from beside it along its facing.
    PlayScene::Inputs attack{};
    attack[0].attack = true;
    attack[0].move = MoveInput{Vec2{0.0f, 1.0f}, 1.0f};
    const usize voices = sounds.voiceCount();
    s32 thrown = 0;
    for (s32 i = 0; i < 40 && scene.missiles().count() == 0; ++i) {
        scene.update(1.0 / 60.0, attack);
        ++thrown;
    }
    REQUIRE(scene.missiles().count() == 1);
    REQUIRE(thrown > 5);
    REQUIRE(scene.animator(0)->recovering());
    REQUIRE(sounds.voiceCount() == voices + 1); // the warrior's throw
    REQUIRE(glm::distance(scene.actor(0)->position(), stood) < 0.5f);
    const PlayerMissiles::Missile& axe = scene.missiles().missile(0);
    REQUIRE(axe.owner == 0);
    REQUIRE(axe.model != nullptr);
    REQUIRE(axe.model->bound());
    const Vec3 facing = scene.actor(0)->facing();
    REQUIRE(glm::dot(glm::normalize(Vec3{axe.velocity.x, 0.0f, axe.velocity.z}), facing) > 0.999f);
    REQUIRE(glm::distance(axe.position, scene.actor(0)->followPoint()) < 6.0f);
    // Held, another follows within a second; let go, the throws stop and the body walks on.
    for (s32 i = 0; i < 60; ++i) {
        scene.update(1.0 / 60.0, attack);
    }
    PlayScene::Inputs walk{};
    walk[0].move = attack[0].move;
    for (s32 i = 0; i < 300; ++i) {
        scene.update(1.0 / 60.0, walk);
    }
    REQUIRE(scene.missiles().count() == 0); // every one stopped by the tower or its time
    REQUIRE_FALSE(scene.animator(0)->throwing());
    REQUIRE(glm::distance(scene.actor(0)->position(), stood) > 2.0f);
    scene.close();
}

TEST_CASE("Sumner greets a player who steps up to him and hands them his scroll of hints",
          "[game][screens][unpacked]") {
    const s32 player = GENERATE(0, 2);
    const auto slot = static_cast<usize>(player);
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    StringTable strings;
    strings.load(test::dataDirectory() / "text", config.text.language);
    test::FakeRenderDevice device;
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "AB";
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{3.3f, 2.1f, -49.0f}; // in the spot before him
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{player, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (s32 i = 0; i < PlayScene::kSpawnTicks + 2 && scene.spawning(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // He greets them at once; the scroll comes two seconds later.
    scene.update(1.0 / 60.0, still);
    scene.update(1.0 / 60.0, still);
    REQUIRE(scene.sumner().playing(SumnerFigure::kWelcomeIndex));
    REQUIRE_FALSE(scene.hints().active());
    for (s32 i = 0; i < 100; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.hints().active());
    for (s32 i = 0; i < 40 && !scene.hints().active(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.hints().active());
    REQUIRE(scene.hints().topics().definition().title == "How Can I Help You?");
    REQUIRE(scene.hints().topics().definition().items.size() == 4);
    REQUIRE(scene.hints().topics().definition().playerLabel ==
            std::format("Player {}", player + 1));

    // The scroll holds play: the stick moves nobody.
    const Vec3 stood = scene.actor(player)->position();
    PlayScene::Inputs walk{};
    walk[slot].move = MoveInput{Vec2{1.0f, 0.0f}, 1.0f};
    scene.update(1.0 / 60.0, walk);
    REQUIRE(scene.actor(player)->position() == stood);

    // Other controller lanes cannot navigate or dismiss this player's scroll.
    PlayScene::Inputs other{};
    other[(slot + 1) % other.size()].menu.select = true;
    other[(slot + 1) % other.size()].menu.down = true;
    other[(slot + 1) % other.size()].menu.back = true;
    scene.update(1.0 / 60.0, other);
    REQUIRE_FALSE(scene.hints().reading());
    REQUIRE(scene.hints().topics().selection() == 0);
    REQUIRE(scene.hints().active());

    // The first topic answers with the first general hint; Back returns to the topics.
    PlayScene::Inputs select{};
    select[slot].menu.select = true;
    PlayScene::Inputs back{};
    back[slot].menu.back = true;
    scene.update(1.0 / 60.0, select);
    REQUIRE(scene.hints().reading());
    REQUIRE(scene.hints().page().definition().title == "A Hint for You");
    REQUIRE(scene.hints().page().definition().body.size() == 1);
    REQUIRE(scene.hints().page().definition().body[0].starts_with("Your precious food"));
    scene.update(1.0 / 60.0, back);
    REQUIRE_FALSE(scene.hints().reading());
    // The guardians' page is titled after the guardian it speaks of.
    PlayScene::Inputs down{};
    down[slot].menu.down = true;
    scene.update(1.0 / 60.0, down);
    scene.update(1.0 / 60.0, select);
    REQUIRE(scene.hints().page().definition().title == "The Lich");
    scene.update(1.0 / 60.0, back);

    // Backing out of the topics burns the scroll; Sumner waves the player off, and the same
    // visit brings no second scroll.
    scene.update(1.0 / 60.0, back);
    REQUIRE(scene.hints().burning());
    for (s32 i = 0; i < 120 && scene.hints().active(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.hints().active());
    REQUIRE(scene.sumner().playing(SumnerFigure::kGoAwayIndex));
    for (s32 i = 0; i < 200; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.hints().active());
    scene.close();
}

TEST_CASE("a figure comes from the costume tier of its level when that is unpacked",
          "[game][screens]") {
    const auto root = test::scratchDirectory("tower-costume-tiers");
    std::filesystem::create_directories(root / "PLAYERS/WAR/BLU00");
    writeTextFile(root / "PLAYERS/WAR/BLU00/objects.json", "{}");
    CharacterSave save;
    save.character = 0;
    save.color = 1;
    save.progress().experience = levelExperience(1);
    REQUIRE(PlayScene::costumeDirectory(root, save).filename() == "BLU00");
    save.progress().experience = levelExperience(25); // no BLU20 unpacked: the untiered one
    REQUIRE(PlayScene::costumeDirectory(root, save).filename() == "BLU");
    std::filesystem::create_directories(root / "PLAYERS/WAR/BLU20");
    writeTextFile(root / "PLAYERS/WAR/BLU20/objects.json", "{}");
    REQUIRE(PlayScene::costumeDirectory(root, save).filename() == "BLU20");
}

TEST_CASE("quitting a level gives up what it gave, keeping lessons and save slots",
          "[game][screens][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName("G1");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave entering;
    entering.name = "AB";
    entering.gold = 40;
    entering.progress().health = 300;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{13.2f, 10.2f, -59.0f};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, std::vector<PartyMember>{{2, entering}}, options));
    // What the pause menu holds: gold and health gained, a lesson learned, a slot saved to.
    std::vector<PartyMember> paused = scene.party();
    paused[0].save.gold = 999;
    paused[0].save.progress().health = 50;
    paused[0].save.helpSeen = {9};
    paused[0].slot = 4;
    const std::vector<PartyMember> left = scene.abandonedParty(paused);
    REQUIRE(left.size() == 1);
    CHECK(left[0].player == 2);
    CHECK(left[0].save.gold == 40);
    CHECK(left[0].save.health() == 300);
    CHECK(left[0].save.helpSeen == std::vector<s32>{9});
    CHECK(left[0].slot == std::optional<usize>{4});
    CHECK_FALSE(left[0].fallen);
    scene.close();
}

TEST_CASE("the tower scene refuses to open without the level", "[game][screens]") {
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.unpackedRoot = test::scratchDirectory("tower-scene-none");
    PlayScene scene;
    REQUIRE_FALSE(scene.open(device, context, world, {}));
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("tower ceremony ghosts draw after Dream World scenery and retain depth testing",
          "[game][screens][promotion][tower-relics][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("PLAYERS/WAR/SFXYEL/animations.json");
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("L1")));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    save.progress().crystals[8] = LevelTriggers::crystalsNeeded(8);
    bool promotion = false;
    SECTION("level promotion") {
        save.progress().promotedLevel = 29;
        promotion = true;
    }
    SECTION("collection congratulations") {
        save.progress().promotedLevel = 30;
        save.progress().relics.addShard(8);
    }
    const std::vector<PartyMember> party{PartyMember{3, save}};
    PlayOptions options;
    options.welcome = false;
    // Back from Dream World: the wizard stands among its portals, inside their horizon sheet,
    // which the level draws translucent after everything solid.
    options.arrivalWorld = 10;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.promotion().active() == promotion);
    REQUIRE(scene.towerRelics().active() == !promotion);
    const PlayScene::Inputs still{};
    for (s32 frame = 0; frame < 500 && awaitingEntrance(scene); ++frame) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(awaitingEntrance(scene));
    REQUIRE(scene.promotion().active() == promotion);
    REQUIRE(scene.towerRelics().active() == !promotion);
    REQUIRE(world.startPoint(8) != nullptr);
    REQUIRE(scene.actor(3) != nullptr);
    REQUIRE(glm::distance(scene.actor(3)->position(), world.startPoint(8)->position) < 5);
    // The wizard's head, by the texture its node's mesh wears.
    ItemArchive& items = world.items();
    const auto wizard = items.trees.find("WIZARD");
    REQUIRE(wizard.has_value());
    const auto& tree = items.trees.tree(*wizard);
    const auto neck = tree.findNode("BODY1_NEC");
    REQUIRE(neck.has_value());
    const auto neckModel = items.models.find(tree.nodes[*neck].object);
    REQUIRE(neckModel.has_value());
    const Texture* head =
        &items.textures.texture(device, items.models.mesh(*neckModel).parts.front().texture);
    // What the level's translucent pass paints with, taken from that pass alone.
    test::FakeRenderDevice deferred;
    world.scene().drawDeferred(deferred, Mat4{1}, CameraFrame::of(scene.viewCamera()));
    std::vector<const Texture*> scenery;
    for (const auto& draw : deferred.draws) {
        if (draw.blend() != BlendMode::Additive) {
            scenery.push_back(draw.texture);
        }
    }
    REQUIRE_FALSE(scenery.empty());
    device.draws.clear();
    scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
    std::optional<usize> firstHead;
    std::optional<usize> lastScenery;
    for (usize i = 0; i < device.draws.size(); ++i) {
        const Texture* texture = device.draws[i].texture;
        if (texture == head && !firstHead.has_value()) {
            firstHead = i;
        }
        if (std::ranges::find(scenery, texture) != scenery.end()) {
            lastScenery = i;
        }
    }
    REQUIRE(firstHead.has_value());
    REQUIRE(lastScenery.has_value());
    // He adds onto the frame without writing depth: scenery blended after him covers him.
    REQUIRE(*lastScenery < *firstHead);
    REQUIRE(device.draws[*firstHead].state.depthTest);
    REQUIRE_FALSE(device.draws[*firstHead].state.depthWrite);
    REQUIRE(device.draws[*firstHead].blend() == BlendMode::Additive);
    scene.close();
}
TEST_CASE("tower portal columns draw after horizon sheets while retaining wall occlusion",
          "[game][screens][visual-parity][unpacked]") {
    const auto root = unpackedRoot();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("L1")));
    ExitPortals probe;
    REQUIRE(probe.bind(device, world.layout(), world.items(), levels, &world.collision()));
    std::optional<Vec3> position;
    for (usize i = 0; i < probe.size(); ++i) {
        if (probe.portal(i).tag == "b1") {
            position = probe.portal(i).position;
        }
    }
    REQUIRE(position.has_value());
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.progress().crystals[2] = LevelTriggers::crystalsNeeded(2);
    const std::vector<PartyMember> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = position;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    bool lit = false;
    for (s32 frame = 0; frame < 300 && !lit; ++frame) {
        scene.update(1.0 / 30, still);
        for (usize i = 0; i < scene.portals().size(); ++i) {
            const auto& portal = scene.portals().portal(i);
            lit = lit || (portal.tag == "b1" && portal.action == ExitPortals::kWaiting);
        }
    }
    REQUIRE(lit);
    const auto camera = CameraFrame::of(scene.viewCamera());
    device.draws.clear();
    scene.portals().draw(device, Mat4{1}, {}, &camera, TreeModel::Pass::Effects);
    std::set<const Texture*> flame;
    for (const auto& draw : device.draws) {
        flame.insert(draw.texture);
        CHECK(draw.state.depthTest);
        CHECK_FALSE(draw.state.depthWrite);
    }
    REQUIRE_FALSE(flame.empty());
    device.draws.clear();
    world.scene().drawDeferred(device, Mat4{1}, camera);
    std::set<const Texture*> background;
    for (const auto& draw : device.draws) {
        background.insert(draw.texture);
    }
    device.draws.clear();
    scene.render(device, makeScreenProjection(640, 448), 640, 448);
    std::optional<usize> firstFlame;
    std::optional<usize> lastBackground;
    for (usize i = 0; i < device.draws.size(); ++i) {
        const auto* texture = device.draws[i].texture;
        if (flame.contains(texture) && !firstFlame.has_value()) {
            firstFlame = i;
        }
        if (background.contains(texture)) {
            lastBackground = i;
        }
    }
    REQUIRE(firstFlame.has_value());
    REQUIRE(lastBackground.has_value());
    CHECK(*lastBackground < *firstFlame);
    scene.close();
}

TEST_CASE("fully unlocked Knight walks both tower wing gates in both directions",
          "[game][screens][tower-wings][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("PLAYERS/KNI/GRE/animations.json");
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/tower-fully-unlocked.json");
    const auto gate = GENERATE(usize{1997}, usize{2031});
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.layout().objects()[gate].name == (gate == 1997 ? "L1TRAPWEASYA" : "L1TRAPWMEDA"));
    const Mat4 closed = tower.scene().worldTransform(gate);
    const Vec3 center{closed[3]};
    const f32 outward = gate == 1997 ? 1.0f : -1.0f;
    const Vec3 direction = glm::normalize(Vec3{closed * Vec4{outward, 0, 0, 0}});
    const auto floor = tower.collision().floorAt(center, 0, 20);
    REQUIRE(floor);
    const Vec3 threshold{center.x, floor->y, center.z};
    // Locked doors still block. The fix must not simply disable gate collision.
    CHECK(glm::distance(tower.collision().resolveWalls(threshold, PlayerActor::kDefaultWidth * 0.5f,
                                                       floor->y + PlayerActor::kFootClearance,
                                                       floor->y + PlayerActor::kDefaultHeight -
                                                           PlayerActor::kFootClearance),
                        threshold) > 0.5f);
    Vec3 start = center - direction * 6.0f;
    const auto startFloor = tower.collision().floorAt(start, 0, 20);
    REQUIRE(startFloor);
    start.y = startFloor->y;
    const GameConfig config;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    context.tower = &tower;
    PlayOptions options;
    options.welcome = false;
    options.position = start;
    PlayScene scene;
    REQUIRE(scene.open(device, context, tower, scenario.partyMembers(), options));
    REQUIRE(tower.triggers().opened(static_cast<s32>(gate)));
    for (s32 tick = 0; tick < 180; ++tick) {
        scene.update(1.0 / 30, {});
    }
    // Run actual gameplay input, including the opponent/body post-collision check;
    // testing PlayerActor alone missed the second check's different probe height.
    for (const f32 sign : {1.0f, -1.0f}) {
        for (s32 tick = 0;
             tick < 90 && glm::dot(scene.actor(0)->position() - center, direction) * sign < 6;
             ++tick) {
            const f32 heading =
                std::atan2(direction.x * sign, direction.z * sign) - scene.viewCamera().yaw;
            PlayScene::Inputs input{};
            input[0].move = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1};
            scene.update(1.0 / 30, input);
        }
        const Vec3 position = scene.actor(0)->position();
        CAPTURE(gate, sign, position.x, position.y, position.z);
        CHECK(glm::dot(position - center, direction) * sign >= 6);
        REQUIRE(tower.triggers().opened(static_cast<s32>(gate)));
    }
    scene.close();
}

} // namespace
