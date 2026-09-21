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
#include "engine/io/File.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/players/CharacterSave.h"
#include "game/players/Progression.h"
#include "game/menu/ScrollBox.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelWorld.h"

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
    int spawnTicks = 0;
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
        for (int i = 0; i < 16; ++i) {
            REQUIRE(scene.update(1.0 / 60.0, inputs) == PlayOutcome::Running);
        }
        scene.update(1.0 / 60.0, accept);
    }
    REQUIRE(actor->position() == spawn);
    // The scroll burns away, then the camera cuts to the crystals as Sumner gestures, and the
    // party stays put for the cut's three hundred ticks.
    REQUIRE(scroll.burning());
    for (int i = 0; i < 60 && scene.intro() == PlayScene::Intro::Scroll; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.intro() == PlayScene::Intro::Crystal);
    scene.update(1.0 / 60.0, still); // the gesture cuts in on the next tick
    REQUIRE(scene.sumner().gesturing());
    REQUIRE(scene.viewCamera().position != scene.camera().camera().position);
    int heldTicks = 0;
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
    REQUIRE(scene.beamAlpha() == 0.0f);
    REQUIRE(scene.viewCamera().position == scene.camera().camera().position);

    // Half a second of walking forward moves the character and the camera follows.
    const Vec3 before = actor->position();
    const Vec3 cameraBefore = scene.camera().camera().position;
    for (int i = 0; i < 30; ++i) {
        REQUIRE(scene.update(1.0 / 60.0, inputs) == PlayOutcome::Running);
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

TEST_CASE("a scenario's options place the party and skip the welcome", "[game][screens][unpacked]") {
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
    REQUIRE(scene.beamAlpha() == 0.0f);
    // Standing among the crystals, the party picks one up on the first step: it counts for
    // the first realm's gate.
    REQUIRE(actor->save().progress().crystals[1] == 0);
    const PlayScene::Inputs still{};
    for (int i = 0; i < PlayScene::kSpawnTicks + 4; ++i) {
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
    REQUIRE(scene.spawning());
    for (int i = 0; i < PlayScene::kSpawnTicks; ++i) {
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
    // Standing at Sumner's lectern, his beam of light comes up over three seconds.
    REQUIRE(scene.open(device, context, world, party, options));
    const Vec3 lectern = scene.sumner().position();
    scene.close();
    options.position = lectern + Vec3{2.0f, 0.0f, 2.0f};
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.beamAlpha() == 0.0f);
    for (int i = 0; i < 120; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.beamAlpha() == Approx(120.0f / PlayScene::kBeamFadeTicks).margin(0.02f));
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
        for (int i = 0; i < PlayScene::kSpawnTicks + 2 && !scene.scroll().active(); ++i) {
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
        for (int i = 0; i < PlayScene::kSpawnTicks + 4 && !scene.scroll().active(); ++i) {
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
        for (int i = 0; i < 400 && scene.scroll().active() && !scene.scroll().burning(); ++i) {
            scene.update(1.0 / 60.0, i % 20 == 19 ? accept : still);
        }
        REQUIRE(scene.scroll().burning());
        REQUIRE(scene.voice() == kNoSound);
        REQUIRE_FALSE(sounds.isPlaying(voice));
        // With the scroll gone, walking on into the gate: the field hums as it thins before
        // the party and falls silent once it has gone. The stick is camera-relative, so the
        // walk first learns which way forward and right take the character.
        for (int i = 0; i < 200 && scene.scroll().active(); ++i) {
            scene.update(1.0 / 60.0, still);
        }
        REQUIRE_FALSE(scene.scroll().active());
        REQUIRE(scene.fieldSound() == kNoSound);
        const Vec3 gate{22.0f, -2.0f, -79.0f};
        auto step = [&](const Vec2& stick, int ticks) {
            PlayScene::Inputs walk{};
            walk[0].move = MoveInput{stick, 1.0f};
            const Vec3 from = actor->position();
            for (int i = 0; i < ticks; ++i) {
                scene.update(1.0 / 60.0, walk);
            }
            return actor->position() - from;
        };
        const Vec3 forward = glm::normalize(step(Vec2{0.0f, 1.0f}, 6));
        const Vec3 right = glm::normalize(step(Vec2{1.0f, 0.0f}, 6));
        for (int i = 0; i < 900 && scene.fieldSound() == kNoSound; ++i) {
            const Vec3 to = gate - actor->position();
            step(glm::normalize(Vec2{glm::dot(to, right), glm::dot(to, forward)}), 1);
        }
        const SoundHandle hum = scene.fieldSound();
        REQUIRE(hum != kNoSound);
        REQUIRE(sounds.isPlaying(hum));
        for (int i = 0; i < 120 && scene.fieldSound() != kNoSound; ++i) {
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
        for (int i = 0; i < PlayScene::kSpawnTicks + 2 && !scene.scroll().active(); ++i) {
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
    for (int i = 0; i < PlayScene::kSpawnTicks + 4; ++i) {
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
        int leavingFrames = 0;
        for (int i = 0; i < 900 && outcome == PlayOutcome::Running; ++i) {
            outcome = scene.update(1.0 / 60.0, still);
            leavingFrames += scene.leaving() ? 1 : 0;
        }
        REQUIRE(outcome == PlayOutcome::Travel);
        // Through the portal, the transition picture took its two seconds to come up.
        REQUIRE(leavingFrames >= 115);
        REQUIRE(leavingFrames <= 125);
        REQUIRE(scene.transition().covering());
        REQUIRE(scene.destination().name == "G1");
        REQUIRE(scene.destination().realmId == 7);
        REQUIRE(scene.party().size() == 1);
        REQUIRE(scene.party()[0].save.gold == 321); // what is carried goes along
    }
    // The fields load in the tower's place, without Sumner, and lead on; with the next level
    // not unpacked their exit brings the party back to the tower.
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
    for (int i = 0; i < 60; ++i) {
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
    for (int i = 0; i < 900 && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
    }
    REQUIRE(outcome == PlayOutcome::Travel);
    const bool nextUnpacked =
        LevelCatalog::unpacked(root, *levels.byTag("g2"));
    REQUIRE(scene.destination().name == (nextUnpacked ? "G2" : "L1"));
    scene.close();
    REQUIRE(world.load(device, root)); // and the tower loads again after it
    REQUIRE(world.isTower());
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
    for (int i = 0; i < 400 && carried.potions.empty(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(carried.keys == 0);          // spent on the lock
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
    int openedWalking = 0;
    for (int heading = 0; heading < 8; ++heading) {
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
        for (int i = 0; i < 240; ++i) {
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
    for (int i = 0; i < 300 && !scene.help().showing(); ++i) {
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
    REQUIRE(scene.actor(0)->save().health() == 200);
    REQUIRE_FALSE(scene.fallen(0));
    scene.hurtPlayer(0, 500.0f, HurtKind::Burn);
    REQUIRE(scene.fallen(0));
    scene.hurtPlayer(0, 500.0f, HurtKind::Burn); // the fallen are past hurting
    PlayOutcome outcome = PlayOutcome::Running;
    for (int i = 0; i < 1200 && outcome == PlayOutcome::Running; ++i) {
        outcome = scene.update(1.0 / 60.0, still);
    }
    REQUIRE(outcome == PlayOutcome::Fallen);
    const std::vector<PartyMember> after = scene.party();
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].fallen);
    REQUIRE(after[0].slot == std::optional<usize>{3});
    REQUIRE(after[0].save.health() == 300); // as it came in
    REQUIRE(after[0].save.helpSeen == std::vector<s32>{HelpMessages::kChestNeedsKey});
    scene.close();
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
    for (int i = 0; i < 600 && scene.actor(0)->save().health() == 400; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.actor(0)->save().health() == 390); // twenty, halved by the level
    // The tick after, the body flinches, and pushing the stick moves it nowhere until it is
    // over; then it walks off.
    PlayScene::Inputs walking{};
    walking[0].move.direction = Vec2{1.0f, 0.0f};
    walking[0].move.magnitude = 1.0f;
    scene.update(1.0 / 60.0, walking);
    REQUIRE(scene.animator(0) != nullptr);
    REQUIRE(scene.animator(0)->action() == PlayerAnimator::Action::HitReact);
    const Vec3 struckAt = scene.actor(0)->position();
    int held = 0;
    while (scene.animator(0)->reacting() && held < 200) {
        REQUIRE(glm::distance(scene.actor(0)->position(), struckAt) < 0.05f);
        scene.update(1.0 / 60.0, walking);
        ++held;
    }
    REQUIRE(held > 10);
    REQUIRE(held < 200);
    for (int i = 0; i < 30; ++i) {
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
    REQUIRE(&scene.viewCamera() == &scene.camera().camera());
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
    for (int i = 0; i < 400 && scene.spawning(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    const Inventory& now = scene.actor(0)->save().progress().inventory;
    // Worn, the speed powerup quickens the character and growth enlarges them.
    scene.update(1.0 / 60.0, still);
    CharacterSave plain;
    plain.name = "CD";
    PlayerActor bare;
    bare.spawn(0, plain, nullptr, Vec3{0.0f}, 0.0f);
    REQUIRE(PlayScene::bodyScale(scene.actor(0)->save(), PowerupEffects::of(now)) ==
            PowerupEffects::kGrowthScale);
    REQUIRE(PlayScene::bodyScale(plain, PowerupEffects{}) == 1.0f);

    // The green potion, taken last, is used first: its acid bursts about the character.
    PlayScene::Inputs use{};
    use[0].usePotion = true;
    for (int i = 0; i < 60 && scene.effects().count() == 0; ++i) {
        scene.update(1.0 / 60.0, use);
    }
    REQUIRE(scene.effects().count() == 1);
    REQUIRE(scene.effects().effect(0).name == "MP_ACID");
    // A warrior's little magic makes it a small one.
    REQUIRE(scene.effects().effect(0).scale > 0.25f);
    REQUIRE(scene.effects().effect(0).scale < 0.5f);
    REQUIRE(now.potions == std::vector<s32>{1});
    // Held on, no second potion goes; released and thrown, the red one flies and bursts.
    for (int i = 0; i < 200; ++i) {
        scene.update(1.0 / 60.0, use);
    }
    REQUIRE(now.potions.size() == 1);
    REQUIRE(scene.effects().count() == 0);
    PlayScene::Inputs toss{};
    toss[0].throwPotion = true;
    scene.update(1.0 / 60.0, still);
    for (int i = 0; i < 60 && scene.missiles().count() == 0; ++i) {
        scene.update(1.0 / 60.0, toss);
    }
    REQUIRE(scene.missiles().count() == 1);
    REQUIRE(scene.missiles().missile(0).potion == 1);
    REQUIRE(now.potions.empty());
    for (int i = 0; i < 240 && scene.effects().count() == 0; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.missiles().count() == 0);
    REQUIRE(scene.effects().count() == 1);
    REQUIRE(scene.effects().effect(0).name == "MP_FIRE");
    // With no potion left the buttons do nothing.
    for (int i = 0; i < 30; ++i) {
        scene.update(1.0 / 60.0, use);
    }
    REQUIRE_FALSE(scene.animator(0)->conjuring());

    // The three way shot throws three axes at once; taken off in the selector, one.
    PlayScene::Inputs attack{};
    attack[0].attack = true;
    for (int i = 0; i < 60 && scene.missiles().count() == 0; ++i) {
        scene.update(1.0 / 60.0, attack);
    }
    REQUIRE(scene.missiles().count() == 3);
    for (int i = 0; i < 300; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    PlayScene::Inputs open{};
    open[0].selector.up = true;
    scene.update(1.0 / 60.0, open);
    for (int i = 0; i < 40 && !scene.selector(0).showing(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.selector(0).showing());
    PlayScene::Inputs left{};
    left[0].selector.left = true;
    for (int i = 0; i < 4 && now.powerups[static_cast<usize>(scene.selector(0).selection())].kind !=
                                 powerup::kWeapon;
         ++i) {
        scene.update(1.0 / 60.0, left);
    }
    scene.update(1.0 / 60.0, open); // up again: off it comes
    REQUIRE(PowerupEffects::of(now).shots() == 1);
    for (int i = 0; i < 60 && scene.missiles().count() == 0; ++i) {
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
    for (int i = 0; i < 400 && scene.spawning(); ++i) {
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
    int thrown = 0;
    for (int i = 0; i < 40 && scene.missiles().count() == 0; ++i) {
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
    REQUIRE(glm::dot(glm::normalize(Vec3{axe.velocity.x, 0.0f, axe.velocity.z}), facing) >
            0.999f);
    REQUIRE(glm::distance(axe.position, scene.actor(0)->followPoint()) < 6.0f);
    // Held, another follows within a second; let go, the throws stop and the body walks on.
    for (int i = 0; i < 60; ++i) {
        scene.update(1.0 / 60.0, attack);
    }
    PlayScene::Inputs walk{};
    walk[0].move = attack[0].move;
    for (int i = 0; i < 300; ++i) {
        scene.update(1.0 / 60.0, walk);
    }
    REQUIRE(scene.missiles().count() == 0); // every one stopped by the tower or its time
    REQUIRE_FALSE(scene.animator(0)->throwing());
    REQUIRE(glm::distance(scene.actor(0)->position(), stood) > 2.0f);
    scene.close();
}

TEST_CASE("Sumner greets a player who steps up to him and hands them his scroll of hints",
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
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{3.3f, 2.1f, -49.0f}; // in the spot before him
    PlayScene scene;
    const std::vector<PartyMember> party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    for (int i = 0; i < PlayScene::kSpawnTicks + 2 && scene.spawning(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    // He greets them at once; the scroll comes two seconds later.
    scene.update(1.0 / 60.0, still);
    scene.update(1.0 / 60.0, still);
    REQUIRE(scene.sumner().playing(SumnerFigure::kWelcomeIndex));
    REQUIRE_FALSE(scene.hints().active());
    for (int i = 0; i < 100; ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.hints().active());
    for (int i = 0; i < 40 && !scene.hints().active(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE(scene.hints().active());
    REQUIRE(scene.hints().topics().definition().title == "How Can I Help You?");
    REQUIRE(scene.hints().topics().definition().items.size() == 4);
    REQUIRE(scene.hints().topics().definition().playerLabel == "Player 1");

    // The scroll holds play: the stick moves nobody.
    const Vec3 stood = scene.actor(0)->position();
    PlayScene::Inputs walk{};
    walk[0].move = MoveInput{Vec2{1.0f, 0.0f}, 1.0f};
    scene.update(1.0 / 60.0, walk);
    REQUIRE(scene.actor(0)->position() == stood);

    // The first topic answers with the first general hint; Back returns to the topics.
    PlayScene::Inputs select{};
    select[0].menu.select = true;
    PlayScene::Inputs back{};
    back[0].menu.back = true;
    scene.update(1.0 / 60.0, select);
    REQUIRE(scene.hints().reading());
    REQUIRE(scene.hints().page().definition().title == "A Hint for You");
    REQUIRE(scene.hints().page().definition().body.size() == 1);
    REQUIRE(scene.hints().page().definition().body[0].starts_with("Your precious food"));
    scene.update(1.0 / 60.0, back);
    REQUIRE_FALSE(scene.hints().reading());
    // The guardians' page is titled after the guardian it speaks of.
    PlayScene::Inputs down{};
    down[0].menu.down = true;
    scene.update(1.0 / 60.0, down);
    scene.update(1.0 / 60.0, select);
    REQUIRE(scene.hints().page().definition().title == "The Lich");
    scene.update(1.0 / 60.0, back);

    // Backing out of the topics burns the scroll; Sumner waves the player off, and the same
    // visit brings no second scroll.
    scene.update(1.0 / 60.0, back);
    REQUIRE(scene.hints().burning());
    for (int i = 0; i < 120 && scene.hints().active(); ++i) {
        scene.update(1.0 / 60.0, still);
    }
    REQUIRE_FALSE(scene.hints().active());
    REQUIRE(scene.sumner().playing(SumnerFigure::kGoAwayIndex));
    for (int i = 0; i < 200; ++i) {
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

} // namespace
