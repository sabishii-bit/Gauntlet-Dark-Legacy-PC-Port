#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/CombatantProjectiles.h"
#include "game/world/PlayerFigure.h"
#include "game/world/PlayerMissiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path assets() {
    const auto root = test::scratchDirectory("replica-projectiles");
    writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(root / "white.png", test::kTinyPng);
    writeTextFile(root / "textures.json",
                  R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"MOVING","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
       "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]},
      {"name":"STILL","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[]}]})");
    test::convertModelFixture(root);
    return root;
}
CombatSnapshot snapshot(u64 tick = 0) {
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.tick = tick;
    state.motion.cameraContinuity = 1;
    return state;
}
void deliver(CombatPlayback& playback, const CombatSnapshot& state) {
    const auto packets = CombatReplica::packets(state);
    REQUIRE(packets);
    for (const auto& packet : *packets) {
        playback.receive(1, packet);
    }
    REQUIRE(playback.latest());
    REQUIRE(CombatPacket::encode(*playback.latest()) == CombatPacket::encode(state));
}
void sameGeometry(std::span<const test::RecordedDraw> actual,
                  std::span<const test::RecordedDraw> expected) {
    REQUIRE_FALSE(actual.empty());
    REQUIRE(actual.size() == expected.size());
    for (usize draw = 0; draw < actual.size(); ++draw) {
        CAPTURE(draw);
        REQUIRE(actual[draw].vertices.size() == expected[draw].vertices.size());
        for (usize i = 0; i < actual[draw].vertices.size(); ++i) {
            const auto& av = actual[draw].vertices[i];
            const auto& bv = expected[draw].vertices[i];
            const auto a = actual[draw].transform * Vec4{av.position, 1};
            const auto b = expected[draw].transform * Vec4{bv.position, 1};
            CHECK(a.x == Approx(b.x).margin(0.0001f));
            CHECK(a.y == Approx(b.y).margin(0.0001f));
            CHECK(a.z == Approx(b.z).margin(0.0001f));
            CHECK(av.color == bv.color);
            CHECK(av.uv.x == Approx(bv.uv.x));
            CHECK(av.uv.y == Approx(bv.uv.y));
        }
        CHECK(actual[draw].state.depthWrite == expected[draw].state.depthWrite);
        CHECK(actual[draw].state.depthTest == expected[draw].state.depthTest);
        CHECK(actual[draw].state.alphaTest == expected[draw].state.alphaTest);
        CHECK(actual[draw].blend() == expected[draw].blend());
    }
}
MissileLaunch launch(const TreeModel* model = nullptr) {
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 0, 0, true};
    MissileLaunch result;
    result.spec = &kSpec;
    result.owner = 3;
    result.position = {0, 3, 0};
    result.velocity = Vec3{0, 0, 20};
    result.model = model;
    result.damage = 40;
    return result;
}
TEST_CASE("native arrival rings retain host geometry materials and timing on independent replicas",
          "[netplay][replica-projectiles][online-arrival][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    ItemArchive hostArchive;
    ItemArchive guestArchive;
    REQUIRE(hostArchive.load(root));
    REQUIRE(guestArchive.load(root));
    ProjectileResources host;
    ProjectileResources guest;
    REQUIRE(host.addTree(2, hostArchive, "STARTFX", hostDevice));
    REQUIRE(guest.addTree(2, guestArchive, "STARTFX", guestDevice));
    LevelArrivalPresentation arrival;
    const std::array<Vec3, 4> positions{Vec3{1, 2, 3}, Vec3{4, 5, 6}, Vec3{7, 8, 9},
                                        Vec3{10, 11, 12}};
    arrival.begin(hostDevice, hostArchive, positions);
    const PlayerMissiles players;
    const EnemyMissiles enemies;
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    const auto textures = guestDevice.texturesCreated;
    for (s32 tick = 0; tick <= LevelArrivalPresentation::kSpawnTicks; ++tick) {
        if (tick > 0) {
            arrival.capturePresentation();
            arrival.animate(1.0f / 60);
            arrival.advance(1, false, {}, {});
        }
        if (tick != 0 && tick != 1 && tick != 12 && tick != 30 && tick != 59 && tick != 60) {
            continue;
        }
        CAPTURE(tick);
        auto state = snapshot(static_cast<u64>(tick));
        REQUIRE(ProjectileCapture::append(state, host, players, enemies, nullptr, &arrival));
        const auto bytes = CombatPacket::encode(state);
        REQUIRE(bytes);
        const auto decoded = CombatPacket::decode(*bytes);
        REQUIRE(decoded);
        REQUIRE(replica.show(*decoded, guest));
        hostDevice.draws.clear();
        guestDevice.draws.clear();
        arrival.drawEffects(hostDevice, Mat4{1}, {}, 1);
        replica.draw(guestDevice, guest, Mat4{1}, {}, CameraFrame{});
        if (tick == LevelArrivalPresentation::kSpawnTicks) {
            CHECK(state.projectiles.empty());
            CHECK(hostDevice.draws.empty());
            CHECK(guestDevice.draws.empty());
        } else {
            REQUIRE(state.projectiles.size() == positions.size());
            sameGeometry(guestDevice.draws, hostDevice.draws);
            for (usize draw = 0; draw < hostDevice.draws.size(); ++draw) {
                const auto& expected = hostDevice.draws[draw];
                const auto& actual = guestDevice.draws[draw];
                CHECK(std::ranges::equal(
                    dynamic_cast<const test::FakeTexture&>(*actual.texture).pixels,
                    dynamic_cast<const test::FakeTexture&>(*expected.texture).pixels));
                CHECK(actual.state.textureBlend == expected.state.textureBlend);
            }
        }
        CHECK(guestDevice.texturesCreated == textures);
    }
}
TEST_CASE("remote weapon models enemy shots and streaks match host geometry without simulation",
          "[netplay][replica-projectiles]") {
    ItemArchive archive;
    REQUIRE(archive.load(assets()));
    const auto tree = archive.trees.find("MOVING");
    REQUIRE(tree);
    test::FakeRenderDevice device;
    TreeModel hostModel;
    TreeModel clientModel;
    REQUIRE(hostModel.bind(archive.trees.tree(*tree), archive.models, archive.textures, device));
    REQUIRE(clientModel.bind(archive.trees.tree(*tree), archive.models, archive.textures, device));
    ProjectileResources host;
    ProjectileResources client;
    REQUIRE(host.addModel(1, hostModel));
    REQUIRE(client.addModel(1, clientModel));
    REQUIRE(host.addStreak(2, device.whiteTexture()));
    REQUIRE(client.addStreak(2, device.whiteTexture()));
    PlayerMissiles players;
    EnemyMissiles enemies;
    auto shot = launch(&hostModel);
    SECTION("weapon model") {
        REQUIRE(players.launch(shot));
    }
    SECTION("player colored streak") {
        shot.model = nullptr;
        shot.streak = {&device.whiteTexture(), Color{40, 180, 255, 200}, 0.5f};
        REQUIRE(players.launch(shot));
    }
    SECTION("enemy projectile retains solid world occlusion") {
        enemies.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {0, 4, 0}, {15, 7, 20}, 1, &hostModel,
                       0);
    }
    players.update(0.1f, nullptr, {});
    enemies.update(0.1f, nullptr, {});
    auto state = snapshot();
    REQUIRE(ProjectileCapture::append(state, host, players, enemies));
    REQUIRE(state.projectiles.size() == 1);
    const auto original = CombatPacket::encode(state);
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    deliver(playback, state);
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(*playback.sample(0), client));
    const auto camera = CameraFrame::at({20, 25, 30});
    players.draw(device, Mat4{1}, {}, &camera);
    enemies.draw(device, Mat4{1}, {}, &camera);
    const auto expected = device.draws;
    const auto textures = device.texturesCreated;
    for (usize frame = 0; frame < 3; ++frame) {
        device.draws.clear();
        replica.draw(device, client, Mat4{1}, {}, camera);
        sameGeometry(device.draws, expected);
    }
    CHECK(device.texturesCreated == textures);
    device.draws.clear();
    replica.draw(device, client, Mat4{1}, {}, camera, TreeModel::Pass::Opaque);
    const auto solidDraws = device.draws.size();
    replica.draw(device, client, Mat4{1}, {}, camera, TreeModel::Pass::Blended);
    sameGeometry(device.draws, expected);
    if (state.projectiles[0].source == ProjectileSource::Streak) {
        CHECK(solidDraws == 0);
    }
    REQUIRE(ProjectileCapture::append(state, host, players, enemies));
    CHECK(CombatPacket::encode(state) == original);
    CHECK(players.takeImpacts().empty());
    CHECK(enemies.takeHits().empty());
    auto removed = snapshot(1);
    REQUIRE(replica.show(removed, client));
    CHECK(replica.count() == 0);
    CHECK_FALSE(replica.show(state, client));
    REQUIRE(replica.begin(2));
    CHECK_FALSE(replica.show(removed, client));
}

TEST_CASE("remote effect meshes carry their authored poses tint fade and billboard transform",
          "[netplay][replica-projectiles]") {
    ItemArchive archive;
    REQUIRE(archive.load(assets()));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees prototype;
    EffectTrees::Setting setting;
    setting.emitParticles = false;
    setting.seconds = 1;
    setting.fadeSeconds = 1;
    setting.unlit = true;
    setting.depthWrite = false;
    setting.additive = true;
    setting.tint = Color{80, 160, 240, 200};
    setting.alpha = GENERATE(1.0f, 0.75f);
    setting.stretch = {1, 2, 3};
    const auto id = effects.startSet(device, archive, "MOVING", {}, setting);
    REQUIRE(id != 0);
    REQUIRE(prototype.startSet(device, archive, "MOVING", {}, setting) != 0);
    ProjectileResources host;
    ProjectileResources client;
    REQUIRE(host.addEffect(1, effects.effect(0), device));
    REQUIRE(client.addEffect(1, prototype.effect(0), device));
    const auto placement = glm::rotate(glm::translate(Mat4{1}, Vec3{4, 7, 9}), 0.7f, Vec3{1, 0, 0});
    effects.placeAt(id, placement, Vec3{0, 1, 3});
    effects.update(0.05f);
    const PlayerMissiles players;
    const EnemyMissiles enemies;
    auto state = snapshot();
    REQUIRE(ProjectileCapture::append(state, host, players, enemies, &effects));
    REQUIRE(state.projectiles.size() == 1);
    CHECK(state.projectiles[0].alpha == Approx(0.95f * setting.alpha));
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    deliver(playback, state);
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(*playback.sample(0), client));
    const auto camera = CameraFrame::at({20, 25, 30});
    effects.draw(device, Mat4{1}, {}, &camera);
    const auto expected = device.draws;
    device.draws.clear();
    replica.draw(device, client, Mat4{1}, {}, camera);
    sameGeometry(device.draws, expected);
    device.draws.clear();
    replica.draw(device, client, Mat4{1}, {}, camera, TreeModel::Pass::Opaque);
    CHECK(device.draws.empty());
    replica.draw(device, client, Mat4{1}, {}, camera, TreeModel::Pass::Blended);
    sameGeometry(device.draws, expected);
    CHECK(prototype.effect(0).player.frame() == 0);
    CHECK(prototype.effect(0).lived == 0);
    const auto identity = effects.effect(0).instance;
    const auto continuity = effects.effect(0).continuity;
    effects.redirect(id, {0, 0, 0}, {0, 0, -20});
    CHECK(effects.effect(0).instance == identity);
    CHECK(effects.effect(0).continuity > continuity);
    effects.snapPresentation(id);
    CHECK(effects.effect(0).continuity > continuity + 1);
    effects.clear();
    REQUIRE(effects.startSet(device, archive, "MOVING", {}, setting) != 0);
    CHECK(effects.effect(0).instance > identity);
}

TEST_CASE("real projectile reflections cut interpolation without reusing cleared identities",
          "[netplay][replica-projectiles]") {
    PlayerMissiles players;
    auto shot = launch();
    shot.multiplayer = MultiplayerMode::Stun;
    REQUIRE(players.launch(shot));
    const auto first = players.missile(0).instance;
    const auto continuity = players.missile(0).continuity;
    const std::array<MissilePlayer, 1> party{{{1, {0, Vec3{0, 0, 4}, 1, 6}, true}}};
    players.update(0.25f, nullptr, {}, party);
    REQUIRE(players.count() == 1);
    CHECK(players.missile(0).instance == first);
    CHECK(players.missile(0).continuity > continuity);
    CHECK(players.missile(0).velocity.z < 0);
    CHECK_FALSE(players.takeImpacts().empty());
    players.clear();
    REQUIRE(players.launch(shot));
    CHECK(players.missile(0).instance > first);

    EnemyMissiles enemies;
    EnemyView shielded;
    shielded.player = 0;
    shielded.position = {0, 0, 10};
    shielded.reflects = true;
    enemies.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 2);
    const auto enemyId = enemies.missile(0).instance;
    for (usize tick = 0; tick < 30 && enemies.count() > 0 && !enemies.missile(0).reflected;
         ++tick) {
        enemies.update(1.0f / 30, nullptr, std::array{shielded});
    }
    REQUIRE(enemies.count() == 1);
    REQUIRE(enemies.missile(0).reflected);
    CHECK(enemies.missile(0).instance == enemyId);
    CHECK(enemies.missile(0).continuity > 1);
    enemies.clear();
    enemies.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 2);
    CHECK(enemies.missile(0).instance > enemyId);
}

TEST_CASE(
    "an effect morph keeps its identity but does not reuse the old animation on a static tree",
    "[netplay][replica-projectiles]") {
    ItemArchive archive;
    REQUIRE(archive.load(assets()));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees prototype;
    EffectTrees::Setting setting;
    setting.seconds = 1;
    setting.morphIn = 0.05f;
    setting.then = "STILL";
    REQUIRE(effects.startSet(device, archive, "MOVING", {}, setting) != 0);
    REQUIRE(prototype.start(device, archive, "STILL", {}));
    ProjectileResources resources;
    REQUIRE(resources.addEffect(1, effects.effect(0), device));
    REQUIRE(resources.addEffect(2, prototype.effect(0), device));
    const PlayerMissiles players;
    const EnemyMissiles enemies;
    auto first = snapshot();
    REQUIRE(ProjectileCapture::append(first, resources, players, enemies, &effects));
    effects.update(0.1f);
    auto last = snapshot(3);
    REQUIRE(ProjectileCapture::append(last, resources, players, enemies, &effects));
    REQUIRE(last.projectiles.size() == 1);
    CHECK(last.projectiles[0].instance == first.projectiles[0].instance);
    CHECK(last.projectiles[0].resource == 2);
    CHECK(last.projectiles[0].animation.generation == 0);
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(last, resources));
    const auto camera = CameraFrame::at({20, 25, 30});
    effects.draw(device, Mat4{1}, {}, &camera);
    const auto expected = device.draws;
    device.draws.clear();
    replica.draw(device, resources, Mat4{1}, {}, camera);
    sameGeometry(device.draws, expected);
}

TEST_CASE("missing projectile assets invalid cursors and overflow fail atomically",
          "[netplay][replica-projectiles]") {
    test::FakeRenderDevice device;
    ProjectileResources resources;
    REQUIRE(resources.addStreak(1, device.whiteTexture()));
    CHECK_FALSE(resources.addStreak(1, device.whiteTexture()));
    CHECK_FALSE(resources.addStreak(0, device.whiteTexture()));
    PlayerMissiles players;
    const EnemyMissiles enemies;
    auto shot = launch();
    shot.streak.texture = &device.whiteTexture();
    REQUIRE(players.launch(shot));
    auto state = snapshot();
    REQUIRE(ProjectileCapture::append(state, resources, players, enemies));
    const auto original = CombatPacket::encode(state);
    const ProjectileResources missing;
    CHECK_FALSE(ProjectileCapture::append(state, missing, players, enemies));
    CHECK(CombatPacket::encode(state) == original);
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(state, resources));
    auto bad = state;
    bad.projectiles[0].resource = 2;
    CHECK_FALSE(replica.show(bad, resources));
    bad = state;
    bad.projectiles[0].source = ProjectileSource::Enemy;
    CHECK_FALSE(replica.show(bad, resources));
    CHECK(replica.count() == 1);
    for (usize i = 0; i < CombatSnapshot::kMaxProjectiles; ++i) {
        REQUIRE(players.launch(shot));
    }
    CHECK_FALSE(ProjectileCapture::append(state, resources, players, enemies));
    CHECK(CombatPacket::encode(state) == original);

    ItemArchive archive;
    REQUIRE(archive.load(assets()));
    EffectTrees effects;
    REQUIRE(effects.start(device, archive, "MOVING", {}));
    REQUIRE(resources.addEffect(2, effects.effect(0), device));
    players.clear();
    REQUIRE(ProjectileCapture::append(state, resources, players, enemies, &effects));
    REQUIRE(replica.show(state, resources));
    bad = state;
    bad.projectiles[0].animation.sequence = 1;
    CHECK_FALSE(replica.show(bad, resources));
    bad = state;
    bad.projectiles[0].animation.frame = 4;
    CHECK_FALSE(replica.show(bad, resources));
    CHECK(replica.count() == 1);
}

TEST_CASE("Jester native thrown weapon and rider remain visible through received checkpoints",
          "[netplay][replica-projectiles][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = 7;
    save.color = 3;
    save.progress().experience = levelExperience(60);
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->missileArchive() != nullptr);
    PlayerMissiles players;
    players.bindVisuals(device);
    auto shot = launch(&figure->missile());
    shot.spec = &MissileSpec::of(save.character);
    shot.archive = figure->missileArchive();
    shot.tree = figure->missileTree();
    shot.riderArchive = shot.archive;
    shot.riderTree = shot.tree;
    REQUIRE(players.launch(shot));
    REQUIRE(players.visuals().count() == 2);
    ProjectileResources resources;
    REQUIRE(resources.addEffect(1, players.visuals().effect(0), device));
    const EnemyMissiles enemies;
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    const auto camera = CameraFrame::at({15, 20, 30});
    for (u64 tick = 0; tick < 5; ++tick) {
        players.update(1.0f / 30, nullptr, {});
        auto state = snapshot(tick);
        REQUIRE(ProjectileCapture::append(state, resources, players, enemies));
        REQUIRE(state.projectiles.size() == 2);
        deliver(playback, state);
        REQUIRE(replica.show(*playback.sample(tick), resources));
        device.draws.clear();
        players.draw(device, Mat4{1}, {}, &camera);
        const auto expected = device.draws;
        device.draws.clear();
        replica.draw(device, resources, Mat4{1}, {}, camera);
        sameGeometry(device.draws, expected);
    }
    CHECK(players.takeImpacts().empty());
}
TEST_CASE("Garm eye projectiles replicate their sloped ribbons without client combat or spawning",
          "[netplay][replica-boss-projectiles][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GARM.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    CombatantAssets stock;
    REQUIRE(stock.load(device, root, bossDefinition("GARM"), 'G'));
    ProjectileResources resources;
    REQUIRE(resources.addCombatant(100, stock, device));
    CHECK_FALSE(resources.addCombatant(100, stock, device));
    CHECK_FALSE(resources.addCombatant(0, stock, device));
    CHECK_FALSE(resources.addCombatant(65535, stock, device));
    EffectTrees effects;
    CombatantProjectiles shots;
    CombatShot shot;
    shot.data = &stock.data;
    shot.damageIndex = 3;
    shot.origin = {0, 16, 0};
    shot.target = Vec3{15, 3, 30};
    usize sounds = 0;
    const auto sound = [&](std::string_view) { ++sounds; };
    shots.launch(shot, stock.archive, device, effects, sound);
    REQUIRE(shots.count() == 1);
    const PlayerMissiles players;
    const EnemyMissiles enemies;
    const auto camera = CameraFrame::of(WorldCamera{{10, 25, 55}, 0.3f, 3.0f, 0});
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(1));
    for (u64 tick = 0; tick < 15; ++tick) {
        effects.update(1.0f / 60);
        shots.update(1.0f / 60, nullptr, {}, device, effects, sound);
        auto state = snapshot(tick);
        REQUIRE(ProjectileCapture::append(state, resources, players, enemies, &effects));
        REQUIRE(state.projectiles.size() == 1);
        CHECK((state.projectiles[0].flags & ProjectileState::kAlong) != 0);
        const auto packet = CombatPacket::encode(state);
        REQUIRE(packet);
        const auto decoded = CombatPacket::decode(*packet);
        REQUIRE(decoded);
        REQUIRE(replica.show(*decoded, resources));
        device.draws.clear();
        effects.draw(device, Mat4{1}, {}, &camera);
        const auto expected = device.draws;
        const auto sounded = sounds;
        const auto textures = device.texturesCreated;
        const auto frame = effects.effect(0).player.frame();
        for (s32 repeat = 0; repeat < 2; ++repeat) {
            device.draws.clear();
            replica.draw(device, resources, Mat4{1}, {}, camera);
            sameGeometry(device.draws, expected);
        }
        CHECK(sounds == sounded);
        CHECK(device.texturesCreated == textures);
        CHECK(effects.effect(0).player.frame() == frame);
        CHECK(shots.takeHits().empty());
        CHECK(shots.takeSummons().empty());
    }
    shots.clear(effects);
}

TEST_CASE("boss projectile catalogs cover birth flight and impact without late resource binding",
          "[netplay][replica-boss-projectiles][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    usize launched = 0;
    usize drawn = 0;
    usize changed = 0;
    for (s32 kind = 34; kind <= 44; ++kind) {
        CAPTURE(kind, bossNameOf(kind));
        test::FakeRenderDevice device;
        CombatantAssets stock;
        const auto family = bossDefinition(bossNameOf(kind));
        REQUIRE(stock.load(device, root, family, 'G'));
        ProjectileResources resources;
        REQUIRE(resources.addCombatant(1, stock, device));
        std::vector<const CritterData*> definitions{&stock.data};
        for (const auto& child : stock.children) {
            definitions.push_back(&child);
        }
        WorldCollision floor;
        CollisionTriangle triangle;
        triangle.normal = {0, 1, 0};
        triangle.vertices = {Vec3{-100, 0, -100}, Vec3{0, 0, 100}, Vec3{100, 0, -100}};
        floor.build({triangle});
        const PlayerMissiles players;
        const EnemyMissiles enemies;
        const auto camera = CameraFrame::at({15, 25, -30});
        for (const auto* data : definitions) {
            for (usize index = 0; index < data->damages().size(); ++index) {
                const auto& damage = data->damages()[index];
                const auto* cue = data->sound(damage.sound);
                if (cue == nullptr || !cue->shows() || (cue->flags & 0x0F000000U) != 0 ||
                    (damage.type != AttackDefinition::kProjectile &&
                     (damage.type != AttackDefinition::kTargetArea ||
                      (damage.flags & 0x4000000U) == 0))) {
                    continue;
                }
                CAPTURE(data->name(), index, cue->tree);
                EffectTrees effects;
                CombatantProjectiles shots;
                CombatShot shot;
                shot.data = data;
                shot.damageIndex = static_cast<s32>(index);
                shot.origin = {0, 4, 0};
                shot.target = Vec3{0, 1, 10};
                shot.endVisual = family.projectileEndVisual;
                shots.launch(shot, stock.archive, device, effects, {}, &floor);
                REQUIRE(shots.count() == 1);
                ++launched;
                ReplicaProjectiles replica;
                REQUIRE(replica.begin(1));
                std::set<u32> seen;
                for (u64 tick = 0; tick < 360 && (shots.count() > 0 || effects.count() > 0);
                     ++tick) {
                    effects.update(1.0f / 30);
                    const std::array<EnemyView, 1> victim{{{0, {0, 0, 10}, 2, 8}}};
                    shots.update(1.0f / 30, &floor, victim, device, effects, {});
                    auto state = snapshot(tick);
                    REQUIRE(
                        ProjectileCapture::append(state, resources, players, enemies, &effects));
                    REQUIRE(replica.show(state, resources));
                    for (const auto& visual : state.projectiles) {
                        seen.insert(visual.resource);
                    }
                    if (tick % 19 == 0 && !state.projectiles.empty()) {
                        const auto textures = device.texturesCreated;
                        device.draws.clear();
                        replica.draw(device, resources, Mat4{1}, {}, camera);
                        drawn += device.draws.empty() ? 0 : 1;
                        CHECK(device.texturesCreated == textures);
                    }
                }
                changed += seen.size() > 1 ? 1 : 0;
                shots.clear(effects);
                effects.clear();
                auto ended = snapshot(400);
                REQUIRE(ProjectileCapture::append(ended, resources, players, enemies, &effects));
                REQUIRE(replica.show(ended, resources));
                CHECK(replica.count() == 0);
            }
        }
    }
    CHECK(launched >= 20);
    CHECK(drawn > 0);
    CHECK(changed >= 10);
}
} // namespace
