#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/ReplicaActors.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path assets() {
    const auto root = test::scratchDirectory("netplay-replica-actors");
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
MotionSnapshot motion(u64 tick = 0) {
    MotionSnapshot result;
    result.epoch = 1;
    result.tick = tick;
    result.cameraContinuity = 1;
    return result;
}
CombatSnapshot capture(const MotionSnapshot& motion, std::span<const PlayerRuntime> players,
                       const Enemies& enemies) {
    const auto result = CombatCapture::capture(motion, players, enemies);
    REQUIRE(result);
    return *result;
}
CombatSnapshot deliver(CombatPlayback& playback, const CombatSnapshot& snapshot) {
    const auto packets = CombatReplica::packets(snapshot);
    REQUIRE(packets);
    for (const auto& packet : *packets) {
        playback.receive(1, packet);
    }
    const auto shown = playback.sample(snapshot.motion.tick);
    REQUIRE(shown);
    REQUIRE(CombatPacket::encode(*shown) == CombatPacket::encode(snapshot));
    return *shown;
}
void sameGeometry(std::span<const test::RecordedDraw> actual,
                  std::span<const test::RecordedDraw> expected) {
    REQUIRE_FALSE(actual.empty());
    REQUIRE(actual.size() == expected.size());
    for (usize draw = 0; draw < actual.size(); ++draw) {
        CAPTURE(draw);
        REQUIRE(actual[draw].vertices.size() == expected[draw].vertices.size());
        for (usize vertex = 0; vertex < actual[draw].vertices.size(); ++vertex) {
            const auto a = actual[draw].transform * Vec4{actual[draw].vertices[vertex].position, 1};
            const auto b =
                expected[draw].transform * Vec4{expected[draw].vertices[vertex].position, 1};
            CHECK(a.x == Approx(b.x).margin(0.0001f));
            CHECK(a.y == Approx(b.y).margin(0.0001f));
            CHECK(a.z == Approx(b.z).margin(0.0001f));
        }
        CHECK(actual[draw].state.depthWrite == expected[draw].state.depthWrite);
        CHECK(actual[draw].blend() == expected[draw].blend());
    }
}
TEST_CASE("replicated enemies draw received bodies without creating gameplay actors",
          "[netplay][replica-actors]") {
    const auto root = assets();
    test::FakeRenderDevice device;
    Enemies host;
    Enemies client;
    host.open(device, root, nullptr, 4, {}, 1);
    client.open(device, root, nullptr, 4, {}, 2);
    REQUIRE(host.loadKind(kGruntKind));
    REQUIRE(client.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.position = {4, 0, 6};
    const auto slot = host.spawn(spawn, {});
    REQUIRE(slot);
    ReplicaActors actors;
    CombatPlayback playback;
    REQUIRE(actors.begin(1));
    REQUIRE(playback.begin(1, 1));
    const auto camera = CameraFrame::at({0, 20, 30});
    const auto original = capture(motion(), {}, host);
    REQUIRE(actors.show(deliver(playback, original), client));
    CHECK(actors.enemyCount() == 1);
    host.draw(device, Mat4{1}, {}, nullptr, nullptr, &camera);
    const auto expected = device.draws;
    device.draws.clear();
    actors.draw(device, client, Mat4{1}, {}, camera, 0, nullptr);
    sameGeometry(device.draws, expected);
    const auto all = device.draws;
    device.draws.clear();
    actors.draw(device, client, Mat4{1}, {}, camera, 0, nullptr, TreeModel::Pass::DepthWriting);
    actors.draw(device, client, Mat4{1}, {}, camera, 0, nullptr, TreeModel::Pass::Effects);
    sameGeometry(device.draws, all);

    host.hurt(*slot, {.damage = 1, .player = 0, .where = std::nullopt});
    const auto hit = capture(motion(1), {}, host);
    REQUIRE(hit.enemies[0].hitFlash);
    REQUIRE(actors.show(deliver(playback, hit), client));
    device.draws.clear();
    actors.draw(device, client, Mat4{1}, {}, camera, 1, &device.whiteTexture());
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws[0].state.maskedTexture == &device.whiteTexture());
    auto recovered = hit;
    recovered.motion.tick = 2;
    recovered.enemies[0].hitFlash = false;
    REQUIRE(actors.show(deliver(playback, recovered), client));
    device.draws.clear();
    actors.draw(device, client, Mat4{1}, {}, camera, 2, &device.whiteTexture());
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws[0].state.maskedTexture == nullptr);
    CHECK_FALSE(host.takeFeedback().empty()); // not consumed by capture or presentation

    auto removed = recovered;
    removed.motion.tick = 3;
    removed.enemies.clear();
    REQUIRE(actors.show(deliver(playback, removed), client));
    CHECK(actors.enemyCount() == 0);
    device.draws.clear();
    actors.draw(device, client, Mat4{1}, {}, camera, 3, nullptr);
    CHECK(device.draws.empty());
    CHECK_FALSE(actors.show(recovered, client)); // stale render cannot resurrect it
    CHECK(client.count() == 0);
    CHECK(client.targets().empty());
    CHECK(client.movementBodies().empty());
    CHECK(client.takeCues().empty());
    CHECK(client.takeBlows().empty());
    CHECK(client.takeLosses().empty());
    CHECK(client.takeFeedback().empty());
}

TEST_CASE("replica presentation hides unavailable assets and clears on epoch changes",
          "[netplay][replica-actors]") {
    test::FakeRenderDevice device;
    Enemies client;
    client.open(device, assets(), nullptr, 4, {}, 1);
    ReplicaActors actors;
    CombatSnapshot snapshot;
    snapshot.motion = motion();
    EnemyCombatState enemy;
    enemy.instance = 1;
    enemy.kind = kGruntKind;
    enemy.health = 50;
    enemy.fullHealth = 50;
    enemy.animation = {0, 0, 1, 0, 1};
    snapshot.enemies.push_back(enemy);
    CHECK_FALSE(actors.show(snapshot, client));
    CHECK_FALSE(actors.begin(0));
    REQUIRE(actors.begin(1));
    REQUIRE(actors.show(snapshot, client));
    const auto textures = device.texturesCreated;
    actors.draw(device, client, Mat4{1}, {}, CameraFrame::at({0, 20, 30}), 0, nullptr);
    CHECK(device.draws.empty());
    CHECK(device.texturesCreated == textures);
    CHECK_FALSE(client.kindLoaded(kGruntKind)); // packets do not initiate file access
    CHECK_FALSE(actors.begin(1));
    REQUIRE(actors.begin(2));
    CHECK(actors.enemyCount() == 0);
    CHECK_FALSE(actors.show(snapshot, client));
    actors.clear();
    REQUIRE(actors.begin(1));
    CHECK_FALSE(actors.setPlayer(0, 1, nullptr));
    CHECK(actors.playerFigure(4) == nullptr);
}

TEST_CASE("received knight poses match authored bodies without advancing a local animator",
          "[netplay][replica-actors][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies resources;
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.character = 5;
    save.color = 3;
    save.progress().health = 1000;
    player.actor.spawn(0, save, nullptr, {0, 0, 0}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    auto visual = PlayerFigure::load(device, root, save, false);
    REQUIRE(visual);
    REQUIRE(visual->familiarTier() == 0);
    const auto revision = visual->animationRevision();
    const auto frame = visual->animator().player().frame();
    ReplicaActors actors;
    CombatPlayback playback;
    REQUIRE(actors.begin(1));
    REQUIRE(playback.begin(1, 1));
    REQUIRE(actors.setPlayer(0, 7, std::move(visual)));
    const auto camera = CameraFrame::at({0, 20, 30});
    bool changed = false;
    std::vector<test::RecordedDraw> previous;
    for (u64 tick = 0; tick < 24; ++tick) {
        player.figure->animate(1, 2, 1.0f / 30);
        auto where = motion(tick);
        where.players[0] = SeatMotion{7, 1, player.actor.position(), player.actor.yaw()};
        const auto snapshot = capture(where, players, resources);
        REQUIRE(actors.show(deliver(playback, snapshot), resources));
        REQUIRE(actors.visiblePlayers() == 1);
        device.draws.clear();
        actors.draw(device, resources, Mat4{1}, {}, camera, static_cast<f32>(tick), nullptr);
        const auto replica = device.draws;
        REQUIRE_FALSE(replica.empty());
        if (!previous.empty()) {
            changed =
                changed || replica[0].vertices[0].position != previous[0].vertices[0].position;
        }
        previous = replica;
        // Both paths must reach the same authored pose after the start transition.
        if (tick > 8) {
            device.draws.clear();
            player.figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, &camera);
            sameGeometry(replica, device.draws);
        }
        const auto* shown = actors.playerFigure(0);
        REQUIRE(shown != nullptr);
        CHECK(shown->animationRevision() == revision);
        CHECK(shown->animator().player().frame() == frame);
        CHECK_FALSE(shown->animator().released());
        CHECK_FALSE(shown->familiarReleased());
    }
    CHECK(changed);
    auto changedSeat = *playback.latest();
    changedSeat.motion.tick += 1;
    changedSeat.motion.players[0]->grant += 1;
    REQUIRE(actors.show(changedSeat, resources));
    CHECK(actors.visiblePlayers() == 0);
    changedSeat.motion.tick += 1;
    changedSeat.motion.players[0]->grant = 7;
    changedSeat.players[0]->life = ReplicaPlayerLife::InTower;
    REQUIRE(actors.show(changedSeat, resources));
    CHECK(actors.visiblePlayers() == 0);
    REQUIRE(actors.begin(2));
    CHECK(actors.visiblePlayers() == 0);
}

TEST_CASE("displaying a remote throw hides its held weapon but cannot release a second shot",
          "[netplay][replica-actors][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies resources;
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.character = 5;
    save.color = 3;
    player.actor.spawn(0, save, nullptr, {0, 0, 0}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    player.figure->setMelee({MeleeRange::Beyond, false, 0});
    ReplicaActors actors;
    CombatPlayback playback;
    REQUIRE(actors.begin(1));
    REQUIRE(playback.begin(1, 1));
    REQUIRE(actors.setPlayer(0, 1, PlayerFigure::load(device, root, save, false)));
    const auto camera = CameraFrame::at({0, 20, 30});
    bool sawRelease = false;
    bool sawRecover = false;
    for (u64 tick = 0; tick < 60; ++tick) {
        player.figure->animate(0, 2, 1.0f / 30, PlayerDeed::Attack);
        sawRelease = sawRelease || player.figure->animator().released();
        auto where = motion(tick);
        where.players[0] = SeatMotion{1, 1, player.actor.position(), player.actor.yaw()};
        const auto snapshot = capture(where, players, resources);
        REQUIRE(actors.show(deliver(playback, snapshot), resources));
        REQUIRE(actors.visiblePlayers() == 1);
        device.draws.clear();
        actors.draw(device, resources, Mat4{1}, {}, camera, static_cast<f32>(tick), nullptr);
        const auto shown = device.draws;
        REQUIRE_FALSE(shown.empty());
        if (player.figure->animator().recovering()) {
            sawRecover = true;
            device.draws.clear();
            player.figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, true, &camera);
            CHECK(shown.size() == device.draws.size()); // exactly the body, no held sword
        }
        const auto* visual = actors.playerFigure(0);
        REQUIRE(visual != nullptr);
        CHECK_FALSE(visual->animator().released());
        CHECK_FALSE(visual->animator().strongReleased());
        CHECK_FALSE(visual->animator().superReleased());
        CHECK_FALSE(visual->animator().meleeStruck());
        CHECK_FALSE(visual->familiarReleased());
    }
    CHECK(sawRelease);
    CHECK(sawRecover);
}
} // namespace
