#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CombatantFixture.h"
#include "game/enemies/SkorneRelics.h"
#include "game/screens/BossSequence.h"
#include "game/world/LevelWorld.h"
#include "game/world/TargetAssist.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Temple Skorne drops four relics in a fixed arc without coin speed scaling", "[skorne]") {
    const auto drops = SkorneRelics::spray({0, 30, 40}, 1);
    REQUIRE(drops[0].name == "BGNTR_IC");
    REQUIRE(drops[1].name == "BMASK_IC");
    REQUIRE(drops[2].name == "BHORN_IC");
    REQUIRE(drops[3].name == "BGNTL_IC");
    for (const auto& drop : drops) {
        REQUIRE(drop.velocity.y == 30);
        REQUIRE(glm::length(drop.velocity) == Catch::Approx(50));
    }
    REQUIRE(drops.front().velocity.x == Catch::Approx(-drops.back().velocity.x));
    REQUIRE(drops.front().velocity.x < drops[1].velocity.x);
    REQUIRE(drops[1].velocity.x < drops[2].velocity.x);
    REQUIRE(SkorneRelics::kStrength == 240);
}

TEST_CASE("Temple Skorne death creates collectable timed relics from the level records",
          "[skorne][boss-sequence][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE2/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE2/animations.json");
    test::unpackedOrSkip("POWERUPS/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E2");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const usize before = world.placedItems().size();
    ItemArchive weapons;
    TextureSet textures;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 2> players;
    BossSequence sequence;
    sequence.bind({device, world, weapons, textures, effects, audio, &catalog});
    CombatSpew spew;
    spew.origin = {0, 0, 0};
    spew.velocity = {0, 30, 40};
    spew.halfAngle = 1;
    sequence.spewCoins(spew, opponents, players);
    REQUIRE(world.placedItems().size() == before + 4);
    constexpr std::array<u32, 4> kFlags{0x4000, 0x2000, 0x1000, 0x8000};
    for (usize i = 0; i < 4; ++i) {
        const auto& item = world.placedItems().item(before + i);
        REQUIRE(item.subtype == 9);
        REQUIRE(item.flags == kFlags[i]);
        REQUIRE(item.strength == 240);
        REQUIRE(item.noGrabSeconds == 2);
        REQUIRE(item.thrown);
        REQUIRE_FALSE(item.takeable());
    }
    sequence.clear();
}

TEST_CASE("Skorne entrance sends three masonry cues across both start animations",
          "[skorne][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELE2/world.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(catalog.byName("E2").has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("E2")));
    REQUIRE(world.skorneArena().size() == 22);
    const auto* mark = world.layout().findLocator(LocatorKind::Boss);
    REQUIRE(mark != nullptr);
    ItemArchive weapons;
    EffectTrees effects;
    AudioMixer mixer(48000);
    SoundPlayer sound(mixer);
    LevelSoundscape audio;
    test::unpackedOrSkip("audio/SKORNE1/sounds.json");
    audio.open(root, &sound, world.audio(), 'E', true);
    audio.updateAmbience({}, {Vec3{100, 0, 0}, Vec3{1, 0, 0}}, 1);
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, -9.75f, 31}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 42);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    s32 audibleFrames = 0;
    f32 leftEnergy = 0;
    f32 rightEnergy = 0;
    for (s32 frame = 0; frame < 600; ++frame) {
        world.update(1.0f / 30);
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
        std::array<f32, 3200> output{}; // a game frame of stereo PCM, not just queued cue names
        mixer.mix(output);
        f32 energy = 0;
        for (const f32 sample : output) {
            energy += std::abs(sample);
        }
        for (usize i = 0; i < output.size(); i += 2) {
            leftEnergy += std::abs(output[i]);
            rightEnergy += std::abs(output[i + 1]);
        }
        audibleFrames += energy > 0.01f ? 1 : 0;
        sound.update();
    }
    REQUIRE(world.skorneArena().phase() == 3);
    CHECK(audibleFrames > 120);
    CHECK(leftEnergy > rightEnergy);
    opponents.close();
    audio.close();
}

TEST_CASE("Skorne entrance taunts attacks and death reach the real sound bank",
          "[skorne][sound][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::unpackedOrSkip("audio/SKORNE1/sounds.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'E');
    REQUIRE(fixture.spawn("SKORNE1", {0, -25.375f, 0}, 0));
    AudioMixer mixer(48000);
    SoundPlayer sound(mixer);
    LevelSoundscape audio;
    const LevelAudioInfo info{.bank = "SKORNE1", .stream = {}};
    audio.open(root, &sound, &info, 'E', true);
    bool battle = false;
    SECTION("idle fallback taunts") {}
    SECTION("attacks followed by death") {
        battle = true;
    }
    EnemyView target;
    target.player = 0;
    target.position = {0, -9.75f, 33};
    target.height = 6;
    target.radius = 1;
    const std::array players{target};
    std::set<std::string> heard;
    s32 taunts = 0;
    s32 lastTaunt = -1000;
    for (s32 frame = 0; frame < 1400; ++frame) {
        if (battle && frame == 1000) {
            EnemyHit hit;
            hit.damage = 2 * fixture.actor.maxHealth();
            fixture.actor.hurt(hit);
        }
        fixture.update(2, 1.0f / 30,
                       battle ? std::span<const EnemyView>{players} : std::span<const EnemyView>{});
        for (const auto& cue : fixture.actor.takeCues()) {
            if (cue.sound.empty()) {
                continue;
            }
            CAPTURE(frame, cue.sound);
            if (cue.sound == "S_SKORN1GEN1") {
                CHECK(frame == 21); // first update advances to animation frame 1
            } else if (cue.sound == "S_SKORN1GEN2") {
                CHECK(frame == 49);
            }
            CHECK(cue.soundPosition == Vec3{0, -25.375f, 0});
            REQUIRE(audio.playAt(cue.sound, cue.soundPosition, cue.attenuated ? 53.0f : 0.0f,
                                 224.0f / 255) != kNoSound);
            if (cue.sound == "S_SKORN1DEATH") {
                CHECK_FALSE(cue.attenuated);
            }
            heard.insert(cue.sound);
            if (cue.sound == "S_SKORN1TAUNT") {
                CHECK(frame - lastTaunt >= 300);
                lastTaunt = frame;
                ++taunts;
            }
        }
        std::array<f32, 3200> output{};
        mixer.mix(output);
        sound.update();
    }
    CHECK(heard.contains("S_SKORN1GEN1"));
    CHECK(heard.contains("S_SKORN1GEN2"));
    CHECK(heard.contains("S_SKORN1GEN3"));
    if (battle) {
        CHECK(heard.contains("S_SKORN1DEATH"));
        CHECK(std::ranges::any_of(
            heard, [](const auto& name) { return name.starts_with("S_SKORN1ATTCK"); }));
    } else {
        CHECK(taunts >= 2);
    }
    audio.close();
}

TEST_CASE("Skorne can be targeted and hit above his buried root with and without Savior",
          "[skorne][target-assist][legend][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELE2/world.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E2");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Bosses bosses;
    bosses.open(device, root, &world.collision(), {}, 'E');
    REQUIRE(bosses.spawn(42, {0, -25.375f, 0}, 0));
    EnemyView player;
    player.player = 0;
    player.position = {0, -9.75f, 33};
    player.height = 6;
    player.radius = 1;
    const std::array players{player};
    for (s32 frame = 0; frame < 600; ++frame) {
        bosses.update(2, 1.0f / 30, players);
    }
    REQUIRE(bosses.view().awake);
    const auto shoot = [&] {
        const Vec3 origin = player.position + Vec3{0, player.height * 0.5f, 0};
        const auto aim = TargetAssist::select(origin, {0, 0, -1}, bosses.targets(),
                                              TargetAssist::kBossRange, &world.collision());
        REQUIRE(aim);
        // The old eight-unit cylinder ended below the platform, at -17.375.
        REQUIRE(aim->y > -17.375f);
        MissileLaunch shot;
        shot.position = origin;
        shot.direction = {0, 0, -1};
        shot.speed = 60;
        shot.reach = TargetAssist::kBossRange;
        shot.spec = &MissileSpec::of(0);
        shot.velocity = TargetAssist::velocity(origin, *aim, shot.speed, shot.spec->weight);
        shot.damage = 100;
        PlayerMissiles missiles;
        REQUIRE(missiles.launch(shot));
        for (s32 frame = 0; frame < 180 && missiles.count() != 0; ++frame) {
            bosses.update(1, 1.0f / 60, players);
            missiles.update(1.0f / 60, &world.collision(), bosses.targets());
        }
        const auto impacts = missiles.takeImpacts();
        REQUIRE(impacts.size() == 1);
        REQUIRE(impacts.front().target == Bosses::kTargetId);
        const f32 health = bosses.view().health;
        EnemyHit hit;
        hit.damage = impacts.front().damage;
        hit.player = 0;
        hit.node = impacts.front().node;
        bosses.hurt(hit, impacts.front().target);
        REQUIRE(bosses.view().health < health);
    };
    shoot();
    REQUIRE(bosses.bringLegend(0));
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(2, 1.0f / 30, players);
    }
    REQUIRE(bosses.legend().thrown());
    bosses.landLegend();
    REQUIRE(bosses.curbed());
    shoot();
}

TEST_CASE("Skorne health and range windows expose his authored attack families",
          "[skorne][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'E');
    REQUIRE(fixture.spawn("SKORNE1", {0, -25.375f, 0}, 0));
    std::set<std::string> attacks;
    std::set<s32> shots;
    bool detachedSlam = false;
    for (const f32 health : {1.0f, 0.6f, 0.15f}) {
        for (const Vec3 target :
             {Vec3{0, -9.75f, 15}, Vec3{0, -9.75f, 33}, Vec3{0, -9.75f, 40}, Vec3{0, -9.75f, 65},
              Vec3{-18, -9.75f, 18}, Vec3{18, -9.75f, 18}}) {
            REQUIRE(fixture.actor.spawn(fixture.assets, 0, {0, -25.375f, 0}, 0, nullptr, {}, 'E'));
            if (health < 1) {
                EnemyHit hit;
                hit.damage = fixture.actor.maxHealth() * (1 - health);
                fixture.actor.hurt(hit);
            }
            EnemyView player;
            player.player = 0;
            player.position = target;
            player.height = 6;
            player.radius = 1;
            const std::array players{player};
            for (s32 frame = 0; frame < 5400; ++frame) {
                fixture.update(2, 1.0f / 30, players);
                if (fixture.actor.moveType() >= 128) {
                    attacks.emplace(fixture.actor.moveName());
                }
                for (const auto& shot : fixture.actor.takeShots()) {
                    shots.insert(shot.damageIndex);
                }
                for (const auto& cue : fixture.actor.takeCues()) {
                    if (cue.tree == "BOSSATK6FX" || cue.tree == "BOSSATK7FX") {
                        REQUIRE_FALSE(cue.follows);
                        REQUIRE(cue.placement.has_value());
                        REQUIRE(cue.life > 0);
                        detachedSlam = true;
                    }
                }
                fixture.actor.takeBlows();
            }
        }
    }
    REQUIRE(attacks == std::set<std::string>{"CLAWL", "GRAB", "FBALL_L", "FBALL_R", "EYEBEAM",
                                             "STOMP", "FOUNTAIN", "WING_ATTACK"});
    REQUIRE(shots == std::set<s32>{4, 5});
    REQUIRE(detachedSlam);
}

TEST_CASE("Savior weakens Skorne on release and death finishes before the victory notice",
          "[skorne][legend][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'E');
    REQUIRE(bosses.spawn(42, {0, -25.375f, 0}, 0));
    REQUIRE(bosses.bringLegend(0));
    EnemyView player;
    player.player = 0;
    player.position = {0, -9.75f, 33};
    player.height = 6;
    player.radius = 1;
    const std::array players{player};
    const f32 health = bosses.view().health;
    bosses.landLegend(); // no effect before the rite asks for the cast
    REQUIRE(bosses.view().health == health);
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(2, 1.0f / 30, players);
    }
    REQUIRE(bosses.legend().thrown());
    REQUIRE(bosses.view().health == health);
    REQUIRE_FALSE(bosses.curbed());
    bosses.landLegend();
    REQUIRE(bosses.curbed());
    REQUIRE(bosses.view().health == Catch::Approx(health * 0.9f + 4)); // ordinary armor applies
    const f32 weakened = bosses.view().health;
    bosses.landLegend();
    REQUIRE(bosses.view().health == weakened);
    for (s32 frame = 0; frame < 1500 && bosses.legend().running(); ++frame) {
        bosses.update(2, 1.0f / 30, players);
    }
    REQUIRE_FALSE(bosses.legend().running());
    REQUIRE_FALSE(bosses.curbed());
    REQUIRE_FALSE(bosses.legend().darkens());
    EnemyHit hit;
    hit.damage = bosses.view().maxHealth * 2;
    hit.player = 0;
    bosses.hurt(hit);
    REQUIRE_FALSE(bosses.view().alive);
    REQUIRE(bosses.present());
    REQUIRE_FALSE(bosses.takeDefeat().has_value());
    const Vec3 rewardOffset = bosses.rewardOffset();
    s32 spews = 0;
    for (s32 frame = 0; frame < 900 && bosses.present(); ++frame) {
        bosses.update(2, 1.0f / 30, players);
        for (const auto& spew : bosses.takeSpews()) {
            REQUIRE(spew.velocity.y > 0);
            ++spews;
        }
    }
    REQUIRE(spews == 1);
    REQUIRE_FALSE(bosses.present());
    REQUIRE(bosses.takeDefeat().has_value());
    REQUIRE_FALSE(bosses.takeDefeat().has_value());
    REQUIRE(bosses.rewardOffset() == rewardOffset);
}
} // namespace
