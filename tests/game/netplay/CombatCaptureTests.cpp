#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/netplay/CombatReplica.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/PlayerHealth.h"
#include "game/screens/PortalDeparture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path assets() {
    const auto root = test::scratchDirectory("netplay-combat-capture");
    const auto archive = root / "MONSTERS/GRU";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"GRU1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30},
                     {"name":"WALK","frames":10,"rate":30},
                     {"name":"HIT1","frames":6,"rate":30},
                     {"name":"HIT2","frames":12,"rate":30},
                     {"name":"DEATH","frames":3,"rate":30}]}]})");
    return root;
}

MotionSnapshot motionFor(std::span<const PlayerRuntime> players, u64 tick = 0) {
    MotionSnapshot result;
    result.epoch = 1;
    result.tick = tick;
    result.cameraContinuity = 1;
    for (const auto& player : players) {
        if (!player.departed) {
            result.players[static_cast<usize>(player.actor.player())] =
                SeatMotion{1, 1, player.actor.position(), player.actor.yaw()};
        }
    }
    return result;
}
CombatSnapshot capture(const MotionSnapshot& motion, std::span<const PlayerRuntime> players,
                       const Enemies& enemies) {
    const auto result = CombatCapture::capture(motion, players, enemies);
    REQUIRE(result);
    return *result;
}
void receive(CombatReplica& replica, const CombatSnapshot& snapshot) {
    const auto chunks = CombatReplica::packets(snapshot);
    REQUIRE(chunks);
    for (usize i = 0; i < chunks->size(); ++i) {
        CHECK(replica.receive(1, (*chunks)[i]) == (i + 1 == chunks->size()
                                                       ? CombatReplica::Admission::Committed
                                                       : CombatReplica::Admission::Pending));
    }
    REQUIRE(replica.latest());
    CHECK(CombatPacket::encode(*replica.latest()) == CombatPacket::encode(snapshot));
}

TEST_CASE("combat capture observes real player damage without replaying it on clients",
          "[netplay][combat-capture]") {
    const Enemies enemies;
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    player.actor.spawn(2, {}, nullptr, {4, 0, 6}, 0);
    player.actor.save().progress().health = 1000;
    PlayerHealth health;
    s32 cues = 0;
    const PlayerHealth::Events events{.block = [](f32, f32) {},
                                      .sound = [&](std::string_view) { ++cues; },
                                      .cry = [&](std::string_view) { ++cues; },
                                      .named = [&](std::string_view, f32) { ++cues; },
                                      .learnBlock = {},
                                      .vibrate = {}};
    CombatReplica replica;
    REQUIRE(replica.begin(1, 1));
    receive(replica, capture(motionFor(players), players, enemies));
    CHECK(replica.latest()->players[2]->health == 1000);
    health.hurt(player, 30.5f, HurtKind::QuietBlow, false, false, 1, events);
    const auto hit = capture(motionFor(players, 1), players, enemies);
    CHECK(hit.players[2]->health == 969.5f);
    CHECK(hit.players[2]->hitFlash);
    const s32 cuesAfterHit = cues;
    receive(replica, hit);
    const auto chunks = CombatReplica::packets(hit);
    REQUIRE(chunks);
    for (const auto& chunk : *chunks) {
        CHECK(replica.receive(1, chunk) == CombatReplica::Admission::Stale);
    }
    CHECK(cues == cuesAfterHit);
    CHECK(capture(motionFor(players, 2), players, enemies).players[2]->health == 969.5f);
    health.hurt(player, 2000, HurtKind::QuietBlow, false, false, 1, events);
    REQUIRE(player.life == PlayerLife::Dying);
    CHECK(player.actor.save().health() == 1); // persistence sentinel is not visible health
    const auto dead = capture(motionFor(players, 3), players, enemies);
    CHECK(dead.players[2]->health == 0);
    CHECK_FALSE(dead.players[2]->damageable);
    receive(replica, dead);
    CHECK(replica.latest()->players[2]->life == ReplicaPlayerLife::Dying);
    player.departed = true;
    receive(replica, capture(motionFor(players, 4), players, enemies));
    CHECK_FALSE(replica.latest()->players[2]);
}

TEST_CASE("enemy identities survive hits and distinguish recycled pool occupants",
          "[netplay][combat-capture]") {
    test::FakeRenderDevice device;
    const auto root = assets();
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.asleep = true;
    const auto slot = enemies.spawn(spawn, {});
    REQUIRE(slot);
    const auto original = capture(motionFor({}), {}, enemies);
    REQUIRE(original.enemies.size() == 1);
    const u64 identity = original.enemies[0].instance;
    CHECK(identity != 0);
    CHECK(original.enemies[0].life == ReplicaEnemyLife::Asleep);
    CombatReplica replica;
    REQUIRE(replica.begin(1, 1));
    receive(replica, original);
    enemies.hurt(*slot, {.damage = 1, .player = 0, .where = std::nullopt});
    const auto hit = capture(motionFor({}, 1), {}, enemies);
    CHECK(hit.enemies[0].instance == identity);
    CHECK(hit.enemies[0].health == enemies.healthOf(*slot));
    CHECK(hit.enemies[0].health < original.enemies[0].health);
    receive(replica, hit);
    CHECK_FALSE(enemies.takeFeedback().empty()); // observing did not consume gameplay outputs
    enemies.hurt(*slot, {.damage = 10000, .player = 0, .where = std::nullopt});
    const auto killed = capture(motionFor({}, 2), {}, enemies);
    CHECK(killed.enemies[0].life == ReplicaEnemyLife::Dying);
    CHECK(killed.enemies[0].health == 0);
    receive(replica, killed);
    CHECK_FALSE(enemies.takeLosses().empty());
    for (s32 i = 0; i < 180 && enemies.observe(*slot); ++i) {
        enemies.update(2, 1.0f / 30, {});
    }
    REQUIRE_FALSE(enemies.observe(*slot));
    receive(replica, capture(motionFor({}, 3), {}, enemies));
    CHECK(replica.latest()->enemies.empty());
    spawn.asleep = false;
    REQUIRE(enemies.spawn(spawn, {}) == slot);
    const auto reborn = capture(motionFor({}, 4), {}, enemies);
    CHECK(reborn.enemies[0].instance > identity);
    receive(replica, reborn);
    spawn.priority = EnemySpawn::Priority::Visible;
    spawn.position.x = 50;
    REQUIRE(enemies.spawn(spawn, {}) == slot); // replacement without an intervening empty tick
    const auto replacement = capture(motionFor({}, 5), {}, enemies);
    CHECK(replacement.enemies[0].instance > reborn.enemies[0].instance);
    receive(replica, replacement);
    enemies.close();
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    REQUIRE(enemies.spawn(spawn, {}));
    CHECK(capture(motionFor({}, 6), {}, enemies).enemies[0].instance >
          replacement.enemies[0].instance);
    CHECK_FALSE(enemies.observe(-1));
    CHECK_FALSE(enemies.observe(Enemies::kMost));
}

TEST_CASE("combat capture rejects mismatched motion and duplicate seats",
          "[netplay][combat-capture]") {
    const Enemies enemies;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    players[1].actor.spawn(1, {}, nullptr, {2, 0, 0}, 0);
    const auto motion = motionFor(players);
    REQUIRE(CombatCapture::capture(motion, players, enemies));
    auto wrong = motion;
    wrong.players[0]->position.x += 1;
    CHECK_FALSE(CombatCapture::capture(wrong, players, enemies));
    wrong = motion;
    wrong.players[0].reset();
    CHECK_FALSE(CombatCapture::capture(wrong, players, enemies));
    CHECK_FALSE(CombatCapture::capture(motion, std::span(players).first(1), enemies));
    players[1].actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    CHECK_FALSE(CombatCapture::capture(motion, players, enemies));
}

TEST_CASE("portal departure captures all survivors but leaves dying teammates on the floor",
          "[netplay][combat-capture][online-departure]") {
    test::FakeRenderDevice device;
    TextureSet empty;
    PortalDeparture departure;
    const Enemies enemies;
    std::array<PlayerRuntime, 4> players;
    for (s32 seat = 0; seat < 4; ++seat) {
        players[static_cast<usize>(seat)].actor.spawn(seat, {}, nullptr,
                                                      {2.0f * static_cast<f32>(seat), 3, 4}, 0);
    }
    players[2].life = PlayerLife::Dying;
    players[3].life = PlayerLife::InTower;
    auto snapshot = CombatCapture::capture(motionFor(players), players, enemies, &departure);
    REQUIRE(snapshot);
    CHECK_FALSE(snapshot->players[0]->portalPhase);
    departure.begin(device, empty);
    for (s32 tick = 0; tick <= PortalDeparture::kTicks; ++tick) {
        snapshot = CombatCapture::capture(motionFor(players, static_cast<u64>(tick)), players,
                                          enemies, &departure);
        REQUIRE(snapshot);
        CHECK(snapshot->players[0]->portalPhase == departure.phase());
        CHECK(snapshot->players[1]->portalPhase == departure.phase());
        CHECK(snapshot->motion.players[0]->position.y == 3);
        CHECK_FALSE(snapshot->players[2]->portalPhase);
        CHECK_FALSE(snapshot->players[3]->portalPhase);
        departure.update(1);
    }
    departure.clear();
    snapshot = CombatCapture::capture(motionFor(players), players, enemies, &departure);
    REQUIRE(snapshot);
    CHECK_FALSE(snapshot->players[0]->portalPhase);
}

TEST_CASE("combat capture preserves authored turbo phase without advancing animation",
          "[netplay][combat-capture][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    const Enemies enemies;
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.character = 5;
    save.color = 3;
    save.progress().health = 1000;
    player.actor.spawn(0, save, nullptr, {0, 0, 0}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    player.figure->animate(0, 2, 1.0f / 30, PlayerDeed::TurboFull);
    REQUIRE(player.figure->animator().damageProtected());
    const auto& animator = player.figure->animator();
    CombatReplica replica;
    REQUIRE(replica.begin(1, 1));
    for (u64 tick = 0; tick < 12; ++tick) {
        const auto revision = player.figure->animationRevision();
        const auto frame = animator.player().frame();
        const auto state = capture(motionFor(players, tick), players, enemies);
        REQUIRE(state.players[0]);
        CHECK(state.players[0]->animation.generation == animator.player().generation());
        CHECK(state.players[0]->animation.sequence == animator.player().sequence());
        CHECK(state.players[0]->animation.action == static_cast<u32>(animator.action()));
        CHECK(state.players[0]->animation.frame == frame);
        CHECK(state.players[0]->animation.transition == animator.player().transition());
        CHECK(state.players[0]->damageable == !animator.damageProtected());
        receive(replica, state);
        CHECK(player.figure->animationRevision() == revision);
        CHECK(animator.player().frame() == frame);
        player.figure->animate(0, 2, 1.0f / 30);
    }
}
} // namespace
