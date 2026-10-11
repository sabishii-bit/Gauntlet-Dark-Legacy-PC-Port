#include <array>
#include <format>
#include <numeric>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/netplay/CharacterProfile.h"
#include "game/players/NameCheats.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/PlayerHealth.h"
#include "game/screens/ReplicaCompanions.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("Pojo injuries use COMMON cries while ordinary characters keep their own voices",
          "[pojo][player-health][audio][assets]") {
    const s32 form = GENERATE(0, 1, 2, 3); // ordinary, pickup, EGG911, disabled pickup
    const s32 injury = GENERATE(0, 1, 2, 3, 4);
    CAPTURE(form, injury);
    struct Injury {
        HurtKind kind;
        f32 damage;
        std::string_view sound;
        u32 id;
        bool queued;
    };
    constexpr std::array kInjuries{Injury{HurtKind::Blow, 70, "S_POJOPAIN", 96, true},
                                   Injury{HurtKind::Burn, 2, "S_POJOPAIN", 96, true},
                                   Injury{HurtKind::Pierce, 2, "S_POJOPAIN", 96, false},
                                   Injury{HurtKind::Gas, 2, "S_POJOPOISON", 98, true},
                                   Injury{HurtKind::Blow, 2000, "S_POJOPOISON", 98, false}};
    const auto& hit = kInjuries[static_cast<usize>(injury)];
    const bool pojo = form == 1 || form == 2;
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    SoundSet common;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    const auto native = common.find(hit.sound);
    REQUIRE(native);
    CHECK(common.entry(*native).id == hit.id);

    const auto soundRoot = test::scratchDirectory("pojo-injury-audio");
    test::writeNativeSoundBank(
        soundRoot / "AUDIO/COMMON.VBK",
        std::format(R"({{"sounds":[{{"name":"{}","id":{},"sequence":[{{"sample":0}}]}}]}})",
                    hit.sound, hit.id),
        std::array{test::NativeSoundSample{48000, std::vector<s16>(4800, -1024)}});
    test::writeNativeSoundBank(
        soundRoot / "AUDIO/WAR.VBK",
        R"({"sounds":[
            {"name":"S_WARPAIN1","id":101,"sequence":[{"sample":0}]},
            {"name":"S_WARPAIN2","id":102,"sequence":[{"sample":0}]},
            {"name":"S_WARPAIN3","id":103,"sequence":[{"sample":0}]},
            {"name":"S_WARPAIN4","id":104,"sequence":[{"sample":0}]},
            {"name":"S_WARDIE1","id":105,"sequence":[{"sample":0}]},
            {"name":"S_WARDIE2","id":106,"sequence":[{"sample":0}]},
            {"name":"S_WARPOISON","id":107,"sequence":[{"sample":0}]}]})",
        std::array{test::NativeSoundSample{48000, std::vector<s16>(4800, 1024)}});
    test::FakeRenderDevice device;
    const ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape audio;
    audio.open(soundRoot, &sounds, nullptr);
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    attacks.bind({device, classes, world, weapons, effects, audio, &sounds, arsenal, dimmer});
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.character = 0;
    save.progress().health = 1000;
    if (form == 2) {
        save.name = "EGG911";
        REQUIRE(applyNameCheats(save));
    } else if (form != 0) {
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
        save.progress().inventory.powerups[0].on = form == 1;
    }
    player.actor.spawn(0, save, nullptr, {}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    REQUIRE(player.figure->voice().load(soundRoot / "AUDIO/WAR"));
    PlayerHealth health;
    health.hurt(player, hit.damage, hit.kind, false, false, 1,
                {.block = [](f32, f32) {},
                 .sound = [](std::string_view) {},
                 .cry = [&](std::string_view cue) { attacks.cry(0, cue, players); },
                 .named = [](std::string_view, f32) {},
                 .learnBlock = {},
                 .vibrate = {}});
    REQUIRE(sounds.voiceCount() == 1);
    CHECK((audio.barkBacklog() > 0) == hit.queued);
    std::array<f32, 2048> samples{};
    mixer.mix(samples);
    // The COMMON test clip is negative; the class-bank clip is positive.
    CHECK(std::accumulate(samples.begin(), samples.end(), 0.0f) * (pojo ? -1 : 1) > 0);
}

TEST_CASE("Pojo is present before the entrance camera and throughout its pan",
          "[pojo][intro][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    const bool cheat = GENERATE(false, true);
    const auto* stage = GENERATE("L1", "G1");
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName(stage);
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    if (cheat) {
        save.name = "EGG911";
        REQUIRE(applyNameCheats(save));
    } else {
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
    }
    PlayScene scene;
    const std::array party{PartyMember{0, save, std::nullopt}};
    REQUIRE(scene.open(device, context, world, party));
    REQUIRE(scene.spawning());
    for (s32 frame = 0; frame < 60; ++frame) {
        CAPTURE(stage, cheat, frame);
        const auto& figure = scene.participants()[0].figure;
        REQUIRE(figure);
        REQUIRE(figure->pojoActive());
        REQUIRE(figure->companion().shown());
        CHECK(figure->companion().attachment(Mat4{1}, "POJOBODY1_HE#1"));
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false);
        CHECK_FALSE(device.draws.empty());
        CHECK(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
}

TEST_CASE("Pojo feather atlas stays clamped at base level on local and guest models",
          "[pojo][materials][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    ItemArchive weapons;
    REQUIRE(archive.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    const auto mesh = archive.models.find("POJOBODY1_L_#0");
    REQUIRE(mesh);
    const u32 slot = archive.models.mesh(*mesh).parts.front().texture;
    const auto& pixels = archive.textures.image(slot);
    // At v=0 repeat blends the clear top with the opaque bottom, creating the frame.
    CHECK(pixels.pixel(pixels.width / 2, 0).a == 0);
    CHECK(pixels.pixel(pixels.width / 2, pixels.height - 1).a == 255);
    const auto* feathers = &archive.textures.texture(device, slot);
    PowerupCompanion local;
    local.choose(device, PowerupCompanion::Kind::Pojo, archive, &weapons);
    CompanionResources guest;
    REQUIRE(guest.bindPowerups(device, archive, &weapons));
    const auto camera = CameraFrame::at({0, 15, 20});
    for (const bool remote : {false, true}) {
        CAPTURE(remote);
        device.draws.clear();
        local.update(1.0f / 30, PlayerAnimator::Action::Breathe, false, false);
        if (remote) {
            CompanionState state;
            state.form = static_cast<u32>(PowerupCompanion::Kind::Pojo);
            state.animation.sequence = PowerupCompanion::kPower;
            state.animation.frame = 1;
            state.animation.generation = 1;
            guest.draw(device, 1, state, Mat4{1}, {}, camera, TreeModel::Pass::All);
        } else {
            local.draw(device, Mat4{1}, Mat4{1}, {}, 1, &camera);
        }
        usize wings = 0;
        usize body = 0;
        for (const auto& draw : device.draws) {
            const auto sampler = draw.state.samplerDescription(TextureDesc{});
            if (draw.texture == feathers) {
                ++wings;
                CHECK(draw.state.clampTexture);
                CHECK_FALSE(draw.state.mipmaps);
                CHECK(sampler.wrap == TextureWrap::ClampToEdge);
                CHECK(sampler.wrapDown() == TextureWrap::ClampToEdge);
                CHECK(draw.state.depthTest);
                CHECK(draw.state.depthWrite);
                CHECK(draw.state.alphaTest > 0);
            } else {
                ++body;
                CHECK_FALSE(draw.state.clampTexture);
                CHECK(draw.state.mipmaps);
                CHECK(sampler.wrap == TextureWrap::Repeat);
                CHECK(sampler.wrapDown() == TextureWrap::Repeat);
            }
        }
        CHECK(wings == 4);
        CHECK(body > 0);
    }
}

TEST_CASE("Pojo restarts its displayed attack on every ranged release across character classes",
          "[pojo][figure][animation][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    CharacterSave save;
    save.name = "EGG911";
    save.character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(save.character);
    REQUIRE(applyNameCheats(save));
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    figure->setCompanionPowerups(device, powerups, save.progress().inventory, nullptr);
    s32 releases = 0;
    for (s32 tick = 0; tick < 240; ++tick) {
        const auto previous = figure->companion().visual(Mat4{1}, 1);
        REQUIRE(previous);
        figure->animate(0, 1, 1.0f / 60, PlayerDeed::Attack);
        if (!figure->animator().released()) {
            continue;
        }
        ++releases;
        const auto shot = figure->companion().visual(Mat4{1}, 1);
        REQUIRE(shot);
        CHECK(shot->sequence == PowerupCompanion::kAttack);
        CHECK(shot->generation > previous->generation);
        CHECK(shot->frame <= 1);
    }
    CHECK(releases >= 3);
}

TEST_CASE("Pojo survives the figure replacement during a tower promotion",
          "[pojo][promotion][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("L1")));
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "EGG911";
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = 29;
    REQUIRE(applyNameCheats(save));
    const std::array party{PartyMember{0, save, std::nullopt}};
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.promotion().active());
    for (s32 frame = 0; frame < 1000 && scene.promotion().active(); ++frame) {
        scene.update(1.0 / 30, {});
        const auto& figure = scene.participants()[0].figure;
        REQUIRE(figure);
        REQUIRE(figure->pojoActive());
        REQUIRE(figure->companion().attachment(Mat4{1}, "POJOBODY1_HE#1"));
    }
    REQUIRE_FALSE(scene.promotion().active());
    CHECK(scene.familiarTier(0) == 1);
    CHECK(scene.actor(0)->save().progress().promotedLevel == 30);
}

TEST_CASE("EGG911 is a permanent form outside all eleven inventory slots", "[pojo][cheats]") {
    CharacterSave save;
    save.name = "EGG911";
    auto& inventory = save.progress().inventory;
    for (usize i = 0; i < inventory.powerups.size(); ++i) {
        inventory.powerups[i] = {60, powerup::kWeapon, 0, 1U << (i + 4), false};
    }
    const auto carried = inventory.powerups;
    REQUIRE(applyNameCheats(save));
    CHECK(inventory.powerups == carried);
    CHECK(inventory.powerupCount() == 11);
    CHECK((PowerupEffects::of(inventory).special & powerup::kPojo) != 0);
    inventory.advance(10000);
    inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
    CHECK(inventory.powerups == carried);
    CHECK(applyNameCheats(save));
    CHECK(inventory.powerups == carried);
    auto loaded = CharacterSave::fromJson(save.toJson());
    CHECK(loaded.progress().inventory == inventory);
    const auto profile = CharacterProfile::capture(save);
    REQUIRE(profile);
    const auto packet = CharacterProfilePacket::encode(*profile);
    REQUIRE(packet);
    const auto received = CharacterProfilePacket::decode(*packet);
    REQUIRE(received);
    CHECK(received->progress.inventory == inventory);
    CHECK(received->gameplayCopy().progress().inventory == inventory);
    loaded.selectClass(5);
    CHECK((PowerupEffects::of(loaded.progress().inventory).special & powerup::kPojo) != 0);
    CHECK(loaded.progress().inventory.powerupCount() == 0);
    CHECK(loaded.progress().inventory.nextHeld(-1, 1) == -1);
}

TEST_CASE("EGG911 migrates old disabled Pojo slots without losing unrelated items",
          "[pojo][cheats]") {
    CharacterSave old;
    old.name = "EGG911";
    old.classes[0].inventory.powerups[3] = {-1, powerup::kSpecial, 0, powerup::kPojo, false};
    old.classes[5].inventory.powerups[7] = {25, powerup::kSpecial, 2,
                                            powerup::kPojo | powerup::kGrowth, false};
    old.classes[5].inventory.keys = 9;
    auto loaded = CharacterSave::fromJson(old.toJson());
    CHECK(loaded.classes[0].inventory.powerupCount() == 0);
    REQUIRE(loaded.classes[5].inventory.powerupCount() == 1);
    const auto& growth = loaded.classes[5].inventory.powerups[0];
    CHECK(growth.flags == powerup::kGrowth);
    CHECK(growth.strength == 25);
    CHECK(growth.charge == 2);
    CHECK_FALSE(growth.on);
    CHECK(loaded.classes[5].inventory.keys == 9);
    for (const auto& progress : loaded.classes) {
        CHECK((PowerupEffects::of(progress.inventory).special & powerup::kPojo) != 0);
    }
    loaded.name = "TEST";
    CHECK_FALSE(applyNameCheats(loaded));
    CHECK((PowerupEffects::of(loaded.progress().inventory).special & powerup::kPojo) == 0);
}

TEST_CASE("Pojo pickups still toggle and expire without making the form permanent",
          "[pojo][powerups]") {
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 2);
    REQUIRE(inventory.powerupCount() == 1);
    CHECK((PowerupEffects::of(inventory).special & powerup::kPojo) != 0);
    inventory.powerups[0].on = false;
    inventory.advance(10);
    CHECK((PowerupEffects::of(inventory).special & powerup::kPojo) == 0);
    CHECK(inventory.powerups[0].strength == 2);
    inventory.powerups[0].on = true;
    inventory.advance(2);
    CHECK(inventory.powerupCount() == 0);
    CHECK((PowerupEffects::of(inventory).special & powerup::kPojo) == 0);
}

TEST_CASE("Pojo replaces the costume and equipped hand and head models in both passes",
          "[pojo][figure][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    CharacterSave save;
    save.character = GENERATE(0, 2, 5, 7);
    CAPTURE(save.character);
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    const auto camera = CameraFrame::at({0, 15, 20});
    figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, &camera);
    const usize original = device.draws.size();
    REQUIRE(original > 0);
    auto& inventory = save.progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kPojo | powerup::kXRay, 0, 2);
    inventory.addPowerup(powerup::kWeapon, powerup::kThunderHammer, -1, -1);
    figure->setCompanionPowerups(device, powerups, inventory, &weapons);
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    figure->holdOnArm(device, &weapons, "RF_SHLD");
    figure->animate(0, 2, 1.0f / 30);
    REQUIRE(figure->pojoActive());
    REQUIRE(figure->companion().shown());
    for (const auto pass :
         {TreeModel::Pass::All, TreeModel::Pass::DepthWriting, TreeModel::Pass::Effects}) {
        device.draws.clear();
        figure->companion().draw(device, Mat4{1}, Mat4{1}, {}, 1, &camera, 1, pass);
        const auto expected = device.draws;
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, &camera, 1, false, pass);
        figure->drawHeadwear(device, powerups, PowerupEffects::of(inventory), Mat4{1}, Mat4{1}, {},
                             1);
        figure->drawGem(device, powerups, "HEAD_HEALTHVAMP", Mat4{1}, Mat4{1}, {}, 1);
        REQUIRE(device.draws.size() == expected.size());
        for (usize i = 0; i < expected.size(); ++i) {
            CHECK(device.draws[i].texture == expected[i].texture);
            REQUIRE(device.draws[i].vertices.size() == expected[i].vertices.size());
            for (usize v = 0; v < expected[i].vertices.size(); ++v) {
                CHECK(device.draws[i].vertices[v].position == expected[i].vertices[v].position);
            }
        }
    }
    inventory = {};
    figure->setCompanionPowerups(device, powerups, inventory, &weapons);
    figure->setWeaponPowerups(device, powerups, weapons, {});
    figure->holdOnArm(device, nullptr, {});
    device.draws.clear();
    figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, &camera);
    CHECK_FALSE(figure->pojoActive());
    CHECK(device.draws.size() == original);
}

TEST_CASE("Pojo throws animated PHOENIX_FBALL from the retail mouth offset for every costume class",
          "[pojo][player-arsenal][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ClassDataSet classes;
    const WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerArsenal arsenal;
    REQUIRE(weapons.load(root / "WEAPONS"));
    REQUIRE(classes.load(root / "PDATA"));
    arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
    for (s32 character = 0; character < kStartingClassCount * 2; ++character) {
        CAPTURE(character);
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
        PlayerActor actor;
        CharacterSave save;
        save.character = character;
        save.color = character % kColorCount;
        actor.spawn(0, save, classes.stats(character), {10, 0, 20}, 0.7f);
        auto figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        auto& inventory = actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
        inventory.addPowerup(powerup::kWeapon, powerup::kThreeWayShot, 0, 60);
        arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, true);
        REQUIRE(arsenal.missiles().count() == 3);
        for (usize shot = 0; shot < arsenal.missiles().count(); ++shot) {
            const auto& missile = arsenal.missiles().missile(shot);
            REQUIRE(missile.effect != 0);
            const auto& visual = arsenal.missiles().visuals().effect(shot);
            CHECK(visual.id == missile.effect);
            CHECK(visual.name == "PHOENIX_FBALL");
            CHECK(missile.spec == &MissileSpec::of(character));
            CHECK(missile.damage > 0);
            CHECK(missile.breaksPotions); // Pojo's own weapon, not a companion projectile.
            const Vec3 expected = actor.followPoint() + Vec3{0, -0.5f, 0} +
                                  actor.facing() * (PlayerMissiles::kMuzzle - 1.25f);
            CHECK(glm::distance(missile.position, expected) == Approx(0).margin(0.0001f));
        }
        device.draws.clear();
        const auto camera = CameraFrame::at({0, 15, 20});
        arsenal.missiles().update(1.0f / 30, nullptr);
        arsenal.missiles().draw(device, Mat4{1}, {}, &camera);
        CHECK_FALSE(device.draws.empty());
        arsenal.missiles().clear();
        arsenal.missiles().bindVisuals(device);
        inventory.powerups[0].on = false;
        arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
        REQUIRE(arsenal.missiles().count() == 1);
        CHECK(arsenal.missiles().missile(0).model == &figure->missile());
        arsenal.missiles().clear();
    }
}
TEST_CASE("Pojo breath uses its animated head and charges only the selected attack source",
          "[pojo][turbo][assets]") {
    const bool carriedBreath = GENERATE(false, true);
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    const ClassDataSet classes;
    LevelWorld world;
    EffectTrees effects;
    LevelSoundscape audio;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const auto soundRoot = test::scratchDirectory("pojo-turbo-audio");
    test::writeNativeSoundBank(
        soundRoot / "AUDIO/COMMON.VBK",
        R"({"sounds":[{"name":"S_POJOTURBO","id":1,
                                 "sequence":[{"sample":0}]}]})",
        std::array{test::NativeSoundSample{48000, std::vector<s16>(4800, 1024)}});
    audio.open(soundRoot, &sounds, nullptr);
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    LevelCatalog levels;
    SoundSet common;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    const auto cue = common.find("S_POJOTURBO");
    REQUIRE(cue);
    CHECK(common.entry(*cue).id == 95); // AudioPlayerTurbo, GUNE5D 8009F550.
    REQUIRE(levels.load(root));
    const auto level = levels.byName("G1");
    REQUIRE(level);
    REQUIRE(world.load(device, root, *level));
    LevelOpponents opponents;
    LevelFixtures fixtures;
    const PlayerAttacks::Targets targets{opponents, fixtures, {}};
    arsenal.bind({device, classes, weapons, world.collision(), effects, audio, &sounds, {}});
    attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.name = "EGG911";
    save.character = 5;
    REQUIRE(applyNameCheats(save));
    player.actor.spawn(0, save, nullptr, {10, 0, 20}, 0.7f);
    if (carriedBreath) {
        player.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kAcidBreath,
                                                            3, 60);
    }
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    player.figure->setCompanionPowerups(device, powerups, save.progress().inventory, &weapons);
    for (s32 tick = 0; tick < 120; ++tick) {
        player.figure->animate(0, 2, 1.0f / 30);
    }
    PlayInput input;
    input.turboAttackPressed = true;
    CHECK(PartyMotion::turboDeed(player, input) == PlayerDeed::None);
    player.turbo.add(40);
    CHECK(PartyMotion::turboDeed(player, input) == PlayerDeed::Breathe);
    player.turbo.add(60);
    CHECK(PartyMotion::turboDeed(player, input) == PlayerDeed::Breathe);
    player.figure->animate(0, 2, 1.0f / 30, PartyMotion::turboDeed(player, input));
    player.pojoTurbo = !carriedBreath;
    REQUIRE(player.figure->animator().itemReleased() == PlayerDeed::Breathe);
    CHECK(player.figure->companion().sequence() == PowerupCompanion::kPower);
    const auto head =
        player.figure->companion().attachment(player.actor.transform(), "POJOBODY1_HE#1");
    REQUIRE(head);
    attacks.useItemAttack(0, players);
    CHECK(sounds.voiceCount() == 1); // Pojo's own cue, on either breath route.
    CHECK(player.glow.level() == (carriedBreath ? 0 : BodyGlow::kStrike));
    dimmer.update(1.0f / 30);
    CHECK((dimmer.offset() < 0) == !carriedBreath);
    CHECK(player.turbo.held() == (carriedBreath ? 100 : 60));
    CHECK(player.actor.save().progress().inventory.powerupCount() == (carriedBreath ? 1 : 0));
    if (carriedBreath) {
        CHECK(player.actor.save().progress().inventory.powerups[0].charge == 2);
    }
    REQUIRE(effects.count() == 1);
    CHECK(effects.effect(0).name == "FIREBREATHE");
    CHECK(glm::distance(Vec3{effects.effect(0).transform()[3]}, Vec3{(*head)[3]}) ==
          Approx(0).margin(0.0001f));
    const auto camera = CameraFrame::at({0, 15, 20});
    bool visible = false;
    for (s32 tick = 0; tick < 30; ++tick) {
        player.pojoTurbo = false; // input edge lasts one tick, presentation must not.
        player.glow.step(1.0f / 30);
        attacks.updateProjectiles(1.0f / 30, players, targets);
        dimmer.update(1.0f / 30);
        effects.update(1.0f / 30);
        device.draws.clear();
        effects.draw(device, Mat4{1}, {}, &camera);
        visible = visible || !device.draws.empty();
        if (tick == 2 && !carriedBreath) {
            CHECK(player.glow.level() == BodyGlow::kStrike);
            CHECK(dimmer.offset() == Approx(-0.4f));
        }
    }
    CHECK(visible);
    for (s32 tick = 0; tick < 180; ++tick) {
        player.glow.step(1.0f / 30);
        attacks.updateProjectiles(1.0f / 30, players, targets);
        dimmer.update(1.0f / 30);
        effects.update(1.0f / 30);
    }
    CHECK(player.glow.level() == 0);
    CHECK(dimmer.offset() == Approx(0));
}
} // namespace
