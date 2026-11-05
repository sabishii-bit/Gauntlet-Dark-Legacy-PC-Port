#include <array>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/audio/SceneSoundEvents.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantProjectiles.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;
static_assert(!std::is_copy_constructible_v<CombatantProjectiles>);
static_assert(!std::is_move_constructible_v<CombatantProjectiles>);

/** Two projectile pools over one synthetic stock: `native` and its `replay`, each with its own
 * effects, compared by the native digests of the pools and of their effects. The prototypes
 * bind the stock's artwork before any pool counts uploads. */
struct Fixture {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    TextureAnimator textures;
    EffectTrees prototypes, effects, replayEffects;
    CombatantProjectiles native, replay;
    NativeStateResources registry;
    std::vector<std::string> sounds;

    explicit Fixture(std::string_view name = "combatant-projectile-checkpoint") {
        const auto root = test::scratchDirectory(name);
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
          {"name":"SHOT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],"sequences":[{"name":"BIRTH","frames":3,"frameRate":30}]},
          {"name":"LOOP","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
          {"name":"HIT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(root / "critter.json",
                      R"({"name":"TEST","types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}],
          "damages":[
            {"type":1,"flags":32,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":0,"sfx":2,"morph":1,"morphEnd":2,"morphLife":0.5},
            {"type":1,"flags":2097184,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
            {"type":1,"flags":67108864,"behaviorFlags":9,"radius":0.5,"maxDistance":2,"damage":1,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
            {"type":8,"flags":67108864,"radius":3,"maxDistance":3,"damage":1,"sfxIndex":0,"morph":1,"morphEnd":2,"morphLife":0.5},
            {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,"minSpeed":30,"maxSpeed":30,"sfxIndex":4,"sfx":2},
            {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,"minSpeed":30,"maxSpeed":30,"sfxIndex":5,"sfx":2},
            {"type":1,"flags":3145760,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2}],
          "sounds":[{"name":"SHOT","flags":8,"levelFormat":"S_%cSHOT"},{"name":"LOOP"},
            {"name":"HIT","flags":16,"levelFormat":"S_%cHIT"},{"name":"LOOP","life":100},
            {"name":"SHOT","flags":131072},{"name":"SHOT","flags":4194304}]})");
        test::convertModelFixture(root);
        REQUIRE(stock.archive.load(root));
        REQUIRE(stock.data.load(root / "critter.json"));
        textures.bind(stock.archive.trees.textureAnimations(), stock.archive.textures, device);
        EffectTrees::Setting setting;
        setting.persistent = true;
        for (const auto* tree : {"SHOT", "LOOP", "HIT"}) {
            REQUIRE(prototypes.startSet(device, stock.archive, tree, {}, setting) != 0);
        }
        REQUIRE(registry.registerArchive(stock.archive, "combatant-projectiles/archive"));
        REQUIRE(registry.registerCritter(stock.data, "combatant-projectiles/critter", 0));
    }
    CombatShot shot(s32 row = 0) const {
        CombatShot value;
        value.data = &stock.data;
        value.critter = 11;
        value.damageIndex = row;
        value.origin = {0, 3, 0};
        value.target = Vec3{0, 3, 30};
        value.realm = 'G';
        return value;
    }
    void launch(s32 row = 0) {
        native.launch(
            shot(row), stock.archive, device, effects,
            [&](const CombatantProjectiles::SoundCue& cue) { sounds.emplace_back(cue.name); });
        REQUIRE(native.count() == 1);
    }
    void step(f32 seconds, std::span<const EnemyView> players = {},
              const WorldCollision* collision = nullptr, std::span<const MissileStop> stops = {}) {
        effects.capturePresentation();
        effects.update(seconds);
        native.update(
            seconds, collision, players, device, effects,
            [&](const CombatantProjectiles::SoundCue& cue) { sounds.emplace_back(cue.name); },
            stops);
    }
    void stepReplay(f32 seconds) {
        replayEffects.capturePresentation();
        replayEffects.update(seconds);
        replay.update(seconds, nullptr, {}, device, replayEffects, {});
    }
    NativeStateDigest::Digest digest(const CombatantProjectiles& pool) const {
        return test::liveDigest(pool, registry.resources());
    }
    void equal() {
        CHECK(digest(native) == digest(replay));
        CHECK(test::liveDigest(effects, registry) == test::liveDigest(replayEffects, registry));
    }
};

TEST_CASE("native combatant projectile history restores RNG contact phases and every output queue",
          "[local-rewind][combatant-missile-local-rewind][native-equality]") {
    Fixture fixture;
    const s32 row = GENERATE(0, 1, 2, 3, 4, 5, 6);
    const bool reflecting = GENERATE(false, true);
    std::array<EnemyView, 2> players{EnemyView{0, {0, 0, 3}, 1, 6}, EnemyView{1, {0, 0, 6}, 1, 6}};
    players[0].reflects = reflecting;
    const std::array opponents{MissileTarget{32, {0, 0, -2}, 1, 8}};
    std::array<CombatantProjectiles::LocalSnapshot, 16> history;
    std::array<EffectTrees::LocalSnapshot, 16> visuals;
    std::array<CombatantProjectiles::LocalSnapshot, 8> expected;
    CombatantProjectiles::LocalSnapshot actual;
    const auto step = [&](CombatantProjectiles& pool, EffectTrees& effects, u32 frame, bool wrong) {
        effects.capturePresentation();
        if (frame % 5 == 0) {
            auto shot = fixture.shot(row);
            shot.target = wrong ? Vec3{5, 9, -10} : Vec3{0, 3, 20};
            pool.launch(shot, fixture.stock.archive, fixture.device, effects, {});
        }
        effects.update(1.0f / 60);
        pool.update(1.0f / 60, nullptr, players, fixture.device, effects, {}, {}, opponents);
    };
    for (u32 frame = 0; frame < 64; ++frame) {
        CAPTURE(frame, row, reflecting);
        const u32 slot = frame % history.size();
        fixture.replay.saveLocal(history[slot]);
        fixture.replayEffects.saveLocal(visuals[slot]);
        const u32 distance = frame % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(fixture.replay, fixture.replayEffects, frame + i, false);
            fixture.replay.saveLocal(expected[i]);
        }
        REQUIRE(fixture.replay.restoreLocal(history[slot]));
        REQUIRE(fixture.replayEffects.restoreLocal(visuals[slot]));
        for (u32 i = 0; i < distance; ++i) {
            step(fixture.replay, fixture.replayEffects, frame + i, true);
        }
        (void)fixture.replay.takeHits();
        (void)fixture.replay.takeRockHits();
        (void)fixture.replay.takeWorldHits();
        (void)fixture.replay.takeGenerators();
        (void)fixture.replay.takeSummons();
        REQUIRE(fixture.replay.canRestoreLocal(history[slot]));
        REQUIRE(fixture.replayEffects.canRestoreLocal(visuals[slot]));
        REQUIRE(fixture.replay.restoreLocal(history[slot]));
        REQUIRE(fixture.replayEffects.restoreLocal(visuals[slot]));
        fixture.equal();
        for (u32 i = 0; i < distance; ++i) {
            step(fixture.native, fixture.effects, frame + i, false);
            step(fixture.replay, fixture.replayEffects, frame + i, false);
            fixture.replay.saveLocal(actual);
            CHECK(fixture.replay.equalLocal(expected[i], actual));
            fixture.equal();
        }
    }
    CHECK(fixture.device.textureUpdates == 0);
}

TEST_CASE("native combatant projectile rewind rejects foreign owners and stale prepared restores",
          "[local-rewind][combatant-missile-local-rewind][native-equality]") {
    CombatantProjectiles a;
    CombatantProjectiles b;
    EffectTrees effects;
    CombatantProjectiles::LocalSnapshot saved;
    CHECK_FALSE(a.equalLocal(saved, saved));
    CHECK_FALSE(a.restoreLocal(saved));
    a.saveLocal(saved);
    CHECK(a.equalLocal(saved, saved));
    CHECK_FALSE(b.equalLocal(saved, saved));
    CHECK_FALSE(b.restoreLocal(saved));
    REQUIRE(a.restoreLocal(saved));
    a.clear(effects);
    CHECK_FALSE(a.equalLocal(saved, saved));
    CHECK_FALSE(a.restoreLocal(saved));
    {
        const CombatantProjectiles temporary;
        temporary.saveLocal(saved);
    }
    CHECK_FALSE(a.equalLocal(saved, saved));
    CHECK_FALSE(a.restoreLocal(saved));
}

TEST_CASE("native combatant projectile equality retains undelivered hit queues",
          "[local-rewind][combatant-missile-local-rewind][native-equality]") {
    Fixture fixture;
    fixture.launch();
    const std::array players{EnemyView{0, {0, 0, 3}, 1, 6}};
    fixture.step(0.3f, players);
    CombatantProjectiles::LocalSnapshot before;
    CombatantProjectiles::LocalSnapshot after;
    fixture.native.saveLocal(before);
    const auto hits = fixture.native.takeHits();
    REQUIRE_FALSE(hits.empty());
    fixture.native.saveLocal(after);
    CHECK_FALSE(fixture.native.equalLocal(before, after));
    REQUIRE(fixture.native.restoreLocal(before));
    fixture.native.saveLocal(after);
    CHECK(fixture.native.equalLocal(before, after));
    CHECK(fixture.native.takeHits() == hits);
}

TEST_CASE("combatant sound phases and linked SFXX rows retain their own identities",
          "[native-combat-audio][combatant-missile-local-rewind]") {
    Fixture fixture;
    const auto root = test::scratchDirectory("combatant-sound-phases");
    writeTextFile(root / "critter.json", R"({"name":"PHASES","types":[{"moveCount":1}],
      "descriptors":[{}],"moves":[{}],"damages":[{"type":1,"behaviorFlags":9,
      "minSpeed":30,"maxSpeed":30,"sfxIndex":0,"sfx":0,"morph":1,"morphEnd":0,
      "morphLife":0.1}],"sounds":[
      {"name":"SHOT","levelFormat":"S_SHARED","link":2},
      {"name":"LOOP","levelFormat":"S_MORPH"},
      {"name":"WHITE","flags":33570816,"life":1,"rate":5,"custom1":500,
       "levelFormat":"S_LINK"}]})");
    CritterData data;
    REQUIRE(data.load(root / "critter.json"));
    auto shot = fixture.shot();
    shot.data = &data;
    const SoundClip clip{24000, 1, std::vector<f32>(24000, 0.125f)};
    const SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "TEST", 1};
    SceneSoundEvents sounds;
    sounds.open(1, nullptr);
    sounds.beginTick(1);
    std::vector<std::tuple<u64, CombatantProjectiles::SoundPhase, s32, std::string>> cues;
    const auto play = [&](const CombatantProjectiles::SoundCue& cue) {
        cues.emplace_back(cue.instance, cue.phase, cue.effect, cue.name);
        auto scope = sounds.scoped({23, static_cast<u64>(cue.effect) + 1, cue.instance},
                                   static_cast<u32>(cue.phase));
        sounds.emit(sequence);
    };
    fixture.native.launch(shot, fixture.stock.archive, fixture.device, fixture.effects, play);
    fixture.effects.update(0.11f);
    fixture.native.update(0.11f, nullptr, {}, fixture.device, fixture.effects, play);
    fixture.effects.update(0.11f);
    fixture.native.update(0.11f, nullptr, {}, fixture.device, fixture.effects, play);
    const auto frame = sounds.endFrame();
    using Phase = CombatantProjectiles::SoundPhase;
    REQUIRE(cues.size() == 5);
    const u64 birth = std::get<0>(cues[0]);
    REQUIRE(birth != 0);
    CHECK((cues[0] == std::tuple{birth, Phase::Launch, 0, std::string{"S_SHARED"}}));
    CHECK((cues[1] == std::tuple{birth, Phase::Launch, 2, std::string{"S_LINK"}}));
    CHECK((cues[2] == std::tuple{birth, Phase::Morph, 1, std::string{"S_MORPH"}}));
    CHECK((cues[3] == std::tuple{birth, Phase::End, 0, std::string{"S_SHARED"}}));
    CHECK((cues[4] == std::tuple{birth, Phase::End, 2, std::string{"S_LINK"}}));
    REQUIRE(frame.finite.events.size() == cues.size());
    for (const auto& event : frame.finite.events) {
        CHECK(event.id.occurrence == 0); // no unrelated phase or linked row advances this cue
    }
    REQUIRE(sounds.reconcileFrames(1, std::span{&frame, 1}));
}

TEST_CASE("corrected combatant impacts do not steal another flight's already presented sound",
          "[native-combat-audio][combatant-missile-local-rewind][scene-sound-events]") {
    Fixture fixture;
    for (const f32 x : {0.0f, 20.0f}) {
        auto shot = fixture.shot();
        shot.origin.x = x;
        shot.target->x = x;
        fixture.native.launch(shot, fixture.stock.archive, fixture.device, fixture.effects, {});
    }
    REQUIRE(fixture.native.count() == 2);
    // A fresh pool numbers its births from one.
    constexpr u64 kFirst = 1;
    constexpr u64 kSecond = 2;
    CombatantProjectiles::LocalSnapshot saved;
    EffectTrees::LocalSnapshot savedEffects;
    fixture.native.saveLocal(saved);
    fixture.effects.saveLocal(savedEffects);
    const SoundClip clip{24000, 1, std::vector<f32>(24000, 0.125f)};
    const SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "TEST", 1};
    AudioMixer mixer(24000);
    SoundPlayer player(mixer);
    SceneSoundEvents sounds;
    sounds.open(7, &player);
    usize commands = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto&) { ++commands; });
    const auto step = [&](u64 tick, bool both) {
        std::array views{EnemyView{0, {0, 0, 0.5f}, 1, 6}, EnemyView{1, {20, 0, 0.5f}, 1, 6}};
        views[0].hidden = !both;
        sounds.beginTick(tick);
        fixture.effects.update(1.0f / 60);
        fixture.native.update(1.0f / 60, nullptr, views, fixture.device, fixture.effects,
                              [&](const CombatantProjectiles::SoundCue& cue) {
                                  auto scope = sounds.scoped(
                                      {23, static_cast<u64>(cue.effect) + 1, cue.instance},
                                      static_cast<u32>(cue.phase));
                                  sounds.emit(sequence);
                              });
        return sounds.endFrame();
    };
    const auto wrong = step(1, true);
    REQUIRE(wrong.finite.events.size() == 2);
    CHECK(wrong.finite.events[0].id.emitter == FrameEventEmitter{23, 3, kFirst});
    CHECK(wrong.finite.events[1].id.emitter == FrameEventEmitter{23, 3, kSecond});
    CHECK(commands == 0);
    const auto published = sounds.reconcileFrames(1, std::span{&wrong, 1});
    REQUIRE(published);
    sounds.dispatch(published);
    REQUIRE(commands > 0);
    const usize presented = commands;
    for (u64 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(fixture.native.restoreLocal(saved));
        REQUIRE(fixture.effects.restoreLocal(savedEffects));
        for (u64 tick = 1; tick <= distance; ++tick) {
            step(tick, true);
        }
        REQUIRE(fixture.native.restoreLocal(saved));
        REQUIRE(fixture.effects.restoreLocal(savedEffects));
        std::vector<SceneSoundEvents::Frame> corrected;
        for (u64 tick = 1; tick <= distance; ++tick) {
            corrected.push_back(step(tick, false));
        }
        REQUIRE(corrected[0].finite.events.size() == 1);
        CHECK(corrected[0].finite.events[0] == wrong.finite.events[1]);
        for (usize frame = 1; frame < corrected.size(); ++frame) {
            CHECK(corrected[frame].finite.events.empty());
        }
        const auto delta = sounds.reconcileFrames(1, corrected);
        REQUIRE(delta);
        CHECK(delta.actions.empty());
        sounds.dispatch(delta);
        CHECK(commands == presented);
    }
    CHECK(sounds.unsupportedCount() == 0);
}

TEST_CASE("combatant hit queues retain retired births when compacted slots are reused",
          "[native-combat-audio][combatant-missile-local-rewind][native-equality]") {
    Fixture fixture;
    fixture.launch();
    // A fresh pool numbers its births from one.
    constexpr u64 kRetired = 1;
    const std::array views{EnemyView{0, {0, 0, 0.5f}, 1, 6}};
    fixture.step(1.0f / 60, views);
    REQUIRE(fixture.native.count() == 0);
    fixture.launch();
    CombatantProjectiles::LocalSnapshot saved;
    CombatantProjectiles::LocalSnapshot drained;
    fixture.native.saveLocal(saved);
    const auto queued = fixture.native.takeHits();
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].instance == kRetired);
    fixture.native.saveLocal(drained);
    CHECK_FALSE(fixture.native.equalLocal(saved, drained));
    REQUIRE(fixture.native.restoreLocal(saved));
    CHECK(fixture.native.takeHits() == queued);
    fixture.native.clear(fixture.effects);
    fixture.launch();
    fixture.step(1.0f / 60, views);
    const auto later = fixture.native.takeHits();
    REQUIRE(later.size() == 1);
    CHECK(later[0].instance > kRetired + 1); // clear never reissues the replacement's birth
}

TEST_CASE("expired shared effects remain pending native projectile cleanup across rewind",
          "[combatant-missile-local-rewind]") {
    Fixture f;
    f.launch(1);
    f.replay.launch(f.shot(1), f.stock.archive, f.device, f.replayEffects, {});
    REQUIRE(f.replay.count() == 1);
    REQUIRE(f.effects.count() == 1);
    REQUIRE(f.replayEffects.count() == 1);
    f.effects.stop(f.effects.effect(0).id);
    f.replayEffects.stop(f.replayEffects.effect(0).id);
    REQUIRE(f.effects.count() == 0);
    REQUIRE(f.native.count() == 1);
    const auto sounds = f.sounds;
    CombatantProjectiles::LocalSnapshot pending;
    EffectTrees::LocalSnapshot pendingEffects;
    f.replay.saveLocal(pending);
    f.replayEffects.saveLocal(pendingEffects);
    f.stepReplay(1.0f / 60);
    REQUIRE(f.replay.restoreLocal(pending));
    REQUIRE(f.replayEffects.restoreLocal(pendingEffects));
    f.equal();
    CHECK(f.sounds == sounds);
    f.step(1.0f / 60);
    f.stepReplay(1.0f / 60);
    f.equal();
    CHECK(f.native.count() == 0);
}

TEST_CASE("combatant projectile rewind retains rock world generator and summon queues",
          "[combatant-missile-local-rewind]") {
    const s32 event = GENERATE(0, 1, 2, 3);
    Fixture f;
    f.launch(event == 2 ? 4 : event == 3 ? 5 : 1);
    if (event == 0) {
        Obstacle cover;
        cover.centre = {0, 0, 2};
        cover.height = 8;
        cover.halfAcross = 5;
        cover.halfAlong = 0.5f;
        const std::array stops{MissileStop{.box = cover, .rock = 3, .rockHealth = 90}};
        f.step(0.1f, {}, nullptr, stops);
    } else if (event == 1) {
        WorldCollision world;
        CollisionTriangle wall;
        wall.normal = {0, 0, -1};
        wall.vertices = {Vec3{-10, 0, 2}, Vec3{0, 20, 2}, Vec3{10, 0, 2}};
        world.build({wall});
        f.step(0.1f, {}, &world);
    } else {
        f.step(0.25f);
    }
    CombatantProjectiles::LocalSnapshot saved;
    EffectTrees::LocalSnapshot savedEffects;
    f.native.saveLocal(saved);
    f.effects.saveLocal(savedEffects);
    const auto rocks = f.native.takeRockHits();
    const auto world = f.native.takeWorldHits();
    const auto generators = f.native.takeGenerators();
    const auto summons = f.native.takeSummons();
    if (event == 0) {
        REQUIRE_FALSE(rocks.empty());
    }
    if (event == 1) {
        REQUIRE_FALSE(world.empty());
    }
    if (event == 2) {
        REQUIRE_FALSE(generators.empty());
    }
    if (event == 3) {
        REQUIRE_FALSE(summons.empty());
    }
    REQUIRE(f.native.restoreLocal(saved));
    REQUIRE(f.effects.restoreLocal(savedEffects));
    CHECK(f.native.takeRockHits() == rocks);
    CHECK(f.native.takeWorldHits() == world);
    CHECK(f.native.takeGenerators() == generators);
    CHECK(f.native.takeSummons() == summons);
}
} // namespace
