#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/WavWriter.h"
#include "game/players/PowerupEffects.h"
#include "game/world/PlayerArsenal.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    ItemArchive weapons;
    WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerArsenal arsenal;
    PlayerActor actor;
    Fixture() {
        actor.spawn(3, {}, nullptr, Vec3{10, 0, 20}, 0);
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
    }
};

TEST_CASE("super shot spends one charge per volley and preserves the last charged shot",
          "[game][items][player-arsenal]") {
    Fixture f;
    PlayerFigure figure;
    auto& inventory = f.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 1, -1);
    inventory.addPowerup(powerup::kWeapon, powerup::kThreeWayShot, 0, 30);
    bool tower = false;
    bool boss = false;
    SECTION("ordinary level") {}
    SECTION("tower conserves charges") {
        tower = true;
    }
    SECTION("boss uses smaller damage multiplier") {
        boss = true;
    }
    f.arsenal.bind({f.device,
                    f.classes,
                    f.weapons,
                    f.collision,
                    f.effects,
                    f.audio,
                    nullptr,
                    {},
                    tower,
                    boss});
    f.arsenal.launchSuperShot(f.actor, &figure);
    REQUIRE(f.arsenal.missiles().count() == 3);
    for (usize i = 0; i < 3; ++i) {
        const auto& shot = f.arsenal.missiles().missile(i);
        CHECK(shot.spec == &MissileSpec::superShot());
        CHECK((shot.flags & powerup::kSuperShot) != 0);
        CHECK(shot.damage == Approx(boss ? 7.5f : 10.0f));
        CHECK(shot.velocity.y == 0);
    }
    CHECK((inventory.powerup(powerup::kWeapon, powerup::kSuperShot) != nullptr) == tower);
    if (!tower) {
        f.arsenal.launchSuperShot(f.actor, &figure);
        REQUIRE(f.arsenal.missiles().count() == 6);
        CHECK((f.arsenal.missiles().missile(3).flags & powerup::kSuperShot) == 0);
    }
}

TEST_CASE("Skorne gauntlets use their own elemental projectiles without consuming super shot",
          "[game][items]") {
    for (const bool left : {false, true}) {
        Fixture f;
        PlayerFigure figure;
        auto& inventory = f.actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial,
                             left ? powerup::kLeftGauntlet : powerup::kRightGauntlet, 0, 60);
        inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 5, -1);
        f.arsenal.launchGauntlet(f.actor, &figure, left);
        REQUIRE(f.arsenal.missiles().count() == 1);
        const auto& missile = f.arsenal.missiles().missile(0);
        CHECK(missile.spec->model == (left ? "BOSSG_ELEC" : "BOSSG_ACID"));
        CHECK((missile.flags & 0xF) == (left ? 2 : 4));
        CHECK(missile.damage == 5);
        CHECK(inventory.powerup(powerup::kWeapon, powerup::kSuperShot)->charge == 5);
    }
}

/** A small visible effect and looping tone keep wall feedback covered without game data. */
std::filesystem::path impactAssets() {
    const auto root = test::scratchDirectory("player-wall-impact");
    const auto weapons = root / "WEAPONS";
    std::filesystem::create_directories(weapons);
    writeTextFile(weapons / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(weapons / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(weapons / "white.png", test::kTinyPng);
    writeTextFile(weapons / "textures.json",
                  R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(weapons / "animations.json", R"({"trees":[
      {"name":"SPARKS","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":10,"frameRate":30}]},
      {"name":"EXPSMALL","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":10,"frameRate":30}]}]})");
    const auto bank = root / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    writeFile(bank / "sample.wav", formats::encodeWav(pcm, 48000, 1));
    writeTextFile(bank / "sounds.json", R"({"sounds":[
      {"index":0,"name":"S_WEAPONHITWOOD","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]},
      {"index":1,"name":"S_3WAYAXE","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]},
      {"index":2,"name":"S_5WAYAXE","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}],
      "samples":[{"index":0,"name":"tone","file":"sample.wav","sampleRate":48000,"frames":4}]})");
    return root;
}

TEST_CASE("weapon wall impacts render once and use the level-selected sound",
          "[game][world][player-arsenal][projectile-impact]") {
    const auto root = impactAssets();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds,
                    "S_WEAPONHITWOOD"});
    MissileImpact impact{.position = {4, 5, 6}};
    SECTION("wall sparks and their sound outlive the projectile") {
        f.arsenal.presentImpact(impact);
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).name == "SPARKS");
        CHECK(f.effects.effect(0).position == impact.position);
        CHECK(f.effects.effect(0).tint.a == 96);
        CHECK(f.effects.effect(0).unlit);
        CHECK_FALSE(f.effects.effect(0).depthWrite);
        CHECK(sounds.voiceCount() == 1);
        std::array<f32, 256> output{};
        mixer.mix(output);
        CHECK(output.back() > 0);
        f.effects.draw(f.device, Mat4{1}, {});
        CHECK_FALSE(f.device.draws.empty());
        f.effects.update(1);
        CHECK(f.effects.count() == 0);
    }
    SECTION("bombs use their own effect") {
        impact.effect = MissileSpec::of(7).impactTree;
        f.arsenal.presentImpact(impact);
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).name == "EXPSMALL");
        CHECK(f.effects.effect(0).tint.a == 255);
        CHECK(sounds.voiceCount() == 1);
    }
    SECTION("target hits do not acquire extra wall feedback") {
        impact.target = 1000;
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 0);
        CHECK(sounds.voiceCount() == 0);
    }
    SECTION("missing artwork does not suppress audio") {
        f.weapons.clear();
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 0);
        CHECK(sounds.voiceCount() == 1);
    }
    SECTION("an unspecified sound is silent rather than an invented fallback") {
        f.arsenal.bind(
            {f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds, {}});
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 1);
        CHECK(sounds.voiceCount() == 0);
    }
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("an obstructed muzzle still produces impact feedback without launching a weapon",
          "[game][world][player-arsenal][projectile-impact]") {
    const auto root = impactAssets();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds,
                    "S_WEAPONHITWOOD"});
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-30, -5, 22}, Vec3{30, -5, 22}, Vec3{0, 40, 22}};
    f.collision.build({wall});
    PlayerFigure figure;
    f.arsenal.launchWeapon(f.actor, &figure, {0, 0, 1}, 1, false);
    CHECK(f.arsenal.missiles().count() == 0);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "SPARKS");
    CHECK(sounds.voiceCount() == 1);
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("spread volleys leave every impact but sound only the centre shot",
          "[game][world][player-arsenal][projectile-impact]") {
    const auto root = impactAssets();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds,
                    "S_WEAPONHITWOOD"});
    s32 shots = 3;
    SECTION("three-way") {
        f.actor.save().progress().inventory.addPowerup(powerup::kWeapon, powerup::kThreeWayShot, 0,
                                                       30);
    }
    SECTION("five-way") {
        shots = 5;
        f.actor.save().progress().inventory.addPowerup(powerup::kWeapon, powerup::kFiveWayShot, 0,
                                                       30);
    }
    PlayerFigure figure;
    f.arsenal.launchWeapon(f.actor, &figure, {0, 0, 1}, 1, true);
    REQUIRE(f.arsenal.missiles().count() == static_cast<usize>(shots));
    CHECK(f.arsenal.missiles().missile(0).wallSound ==
          (shots == 3 ? MissileWallSound::ThreeWay : MissileWallSound::FiveWay));
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-100, -10, 30}, Vec3{100, -10, 30}, Vec3{0, 100, 30}};
    f.collision.build({wall});
    f.arsenal.missiles().update(1, &f.collision);
    REQUIRE(f.arsenal.missiles().count() == 0);
    const auto impacts = f.arsenal.missiles().takeImpacts();
    REQUIRE(impacts.size() == static_cast<usize>(shots));
    for (const auto& impact : impacts) {
        f.arsenal.presentImpact(impact);
    }
    CHECK(f.effects.count() == static_cast<usize>(shots));
    CHECK(sounds.voiceCount() == 1);
    CHECK(f.arsenal.missiles().takeImpacts().empty());
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("player arsenal throws the next potion from its owner's hand",
          "[game][world][player-arsenal]") {
    Fixture f;
    f.actor.save().progress().inventory.addPotions(2, 1);
    f.actor.save().progress().inventory.addPotions(4, 1);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto& missile = f.arsenal.missiles().missile(0);
    REQUIRE(missile.owner == 3);
    REQUIRE(missile.potion == 4);
    REQUIRE(missile.position == Vec3{10, 4, 22});
    REQUIRE(missile.velocity.y == Approx(5 * 0.707f));
    REQUIRE(missile.velocity.z == Approx(5 * 0.707f));
    REQUIRE(missile.potency == Approx(0.75f * f.arsenal.magicPowerOf(f.actor)));
    REQUIRE(missile.model != nullptr);
    REQUIRE(f.actor.save().progress().inventory.nextPotion() == 2);
    f.arsenal.usePotion(f.actor);
    REQUIRE(f.actor.save().progress().inventory.potions.empty());
    REQUIRE(f.arsenal.missiles().count() == 1); // Immediate potion doesn't launch a bottle.
}

TEST_CASE("assisted weapon launch aims from its actual muzzle without retaining a lock",
          "[game][world][player-arsenal][target-assist]") {
    Fixture f;
    PlayerFigure figure;
    const Vec3 target{16, 7, 40};
    f.arsenal.launchWeapon(f.actor, &figure, Vec3{0, 0, 1}, 1, false, target);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto first = f.arsenal.missiles().missile(0);
    const Vec3 offset = target - first.position;
    const f32 time =
        std::hypot(offset.x, offset.z) / std::hypot(first.velocity.x, first.velocity.z);
    const Vec3 reached = first.position + first.velocity * time -
                         Vec3{0, 0.5f * first.spec->weight * time * time, 0};
    REQUIRE(glm::distance(reached, target) == Approx(0).margin(0.0001f));
    REQUIRE(first.velocity.x > 0);
    // No selected target preserves the ordinary forward throw (no sticky lock).
    f.arsenal.launchWeapon(f.actor, &figure, Vec3{0, 0, 1}, 1, false);
    REQUIRE(f.arsenal.missiles().count() == 2);
    REQUIRE(f.arsenal.missiles().missile(1).velocity.x == 0);
}

TEST_CASE("player arsenal tolerates missing artwork and clears projectiles before rebinding",
          "[game][world][player-arsenal]") {
    Fixture f;
    f.arsenal.launchWeapon(f.actor, nullptr, Vec3{0, 0, 1}, 1, true);
    REQUIRE(f.arsenal.missiles().count() == 0);
    f.actor.save().progress().inventory.addPotions(1, 2);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
    REQUIRE(f.arsenal.missiles().count() == 0);
    REQUIRE(f.arsenal.missiles().takeImpacts().empty());
    f.arsenal.clear();
    f.arsenal.clear();
    f.arsenal.throwPotion(f.actor);
    f.arsenal.usePotion(f.actor);
    REQUIRE(f.actor.save().progress().inventory.potions.size() == 1);
    REQUIRE(f.arsenal.missiles().count() == 0);
}
} // namespace
