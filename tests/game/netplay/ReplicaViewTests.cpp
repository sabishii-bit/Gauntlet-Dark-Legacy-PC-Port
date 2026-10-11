#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/CursorAim.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/ReplicaView.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("replica viewport uses the local display without host aspect masks",
          "[netplay][replica-view]") {
    const auto wide = ReplicaView::viewport(1920, 1080);
    REQUIRE(wide);
    CHECK(wide->x == 0);
    CHECK(wide->y == 0);
    CHECK(wide->width == Approx(1920));
    CHECK(wide->height == Approx(1080));
    const auto narrow = ReplicaView::viewport(800, 600);
    REQUIRE(narrow);
    CHECK(narrow->x == 0);
    CHECK(narrow->y == 0);
    CHECK(narrow->width == Approx(800));
    CHECK(narrow->height == Approx(600));
    CHECK(ReplicaView::viewport(640, 448) == Rect{0, 0, 640, 448});
    for (const f32 bad : {0.0f, -1.0f, std::numeric_limits<f32>::infinity(),
                          std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(ReplicaView::viewport(bad, 600));
        CHECK_FALSE(ReplicaView::viewport(800, bad));
    }
    CHECK_FALSE(ReplicaView::viewport(65537, 600));
}

TEST_CASE("replica view stays blank until an entire trusted sample can be drawn",
          "[netplay][replica-view][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    world.setPlayerCount(1);
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    ProjectileResources resources;
    PickupResources pickups;
    REQUIRE(pickups.bind(device, world.placedItems().archives()));
    FixtureResources fixtures;
    const Generators generators;
    const SafeRocks rocks;
    const Breakables barrels;
    const Traps traps;
    const Rubble rubble;
    REQUIRE(fixtures.bind(device, world.placedItems().archives(), generators, rocks));
    MatchContext context;
    context.epoch = 1;
    context.scene = 1;
    context.owners = {1, 0, 0, 0};
    context.grants = {1, 0, 0, 0};
    ReplicaView view;
    CHECK_FALSE(view.cursorAim(0, {0.5f, 0.5f}));
    const Mat4 projection = glm::orthoLH_ZO(0.0f, 1920.0f, 1080.0f, 0.0f, 0.0f, 1.0f);
    view.draw(device, projection, 1920, 1080, 0);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].color == Color::black());
    CHECK_FALSE(device.draws[0].state.depthTest);
    const LevelWorld empty;
    CHECK_FALSE(view.begin(context, empty, enemies, resources, pickups, fixtures));
    REQUIRE(view.begin(context, world, enemies, resources, pickups, fixtures));
    CHECK_FALSE(view.begin(context, world, enemies, resources, pickups, fixtures));

    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.progress().experience = levelExperience(80);
    save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    player.actor.spawn(0, save, nullptr, {}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    player.figure->setCompanionPowerups(device, world.powerups(), save.progress().inventory,
                                        &weapons);
    player.figure->animate(0, 1, 1.0f / 30);
    MotionSnapshot motion;
    motion.epoch = 1;
    motion.tick = 5;
    motion.cameraContinuity = 1;
    motion.camera = {{0, 10, -20}, 0.3f, 0, 0};
    motion.aspect = 4.0f / 3;
    motion.players[0] = SeatMotion{1, 1, {}, 0};
    auto captured = CombatCapture::capture(motion, players, enemies);
    REQUIRE(captured);
    captured->geometry = world.scene().geometry();
    REQUIRE(PickupCapture::append(*captured, world.placedItems(), pickups));
    const Chests chests;
    const LockedGates gates;
    REQUIRE(FixtureCapture::append(
        *captured, fixtures,
        {chests, gates, world.triggers(), generators, barrels, traps, rocks, rubble}));
    REQUIRE_FALSE(captured->fixtures.empty());
    REQUIRE_FALSE(captured->pickups.empty());
    CHECK_FALSE(view.show(*captured)); // do not draw a partial party
    CHECK(view.shown() == nullptr);
    REQUIRE(view.setPlayer(0, PlayerFigure::load(device, root, save, false)));
    CHECK_FALSE(view.show(*captured)); // unknown timed companion cannot trigger file loading
    REQUIRE(view.bindCompanions(device, world.powerups(), &weapons));
    const PartyHud hud;
    captured->hud = HudCapture::capture(players, hud, true);
    REQUIRE(captured->hud);
    CHECK_FALSE(view.show(*captured)); // HUD artwork is also loaded before snapshot admission.
    REQUIRE(view.loadHud(device, root, nullptr));
    GameConfig video;
    video.camera.horizontalFovDegrees = 85;
    SECTION("first gameplay sample with post-processing off") {}
    SECTION("guest enables its own full post-processing chain") {
        video.display.bloom = true;
        video.display.ambientOcclusion = true;
        video.display.depthOfField = true;
    }
    SECTION("host projection changes do not change local presentation") {
        captured->motion.aspect = 21.0f / 9;
        captured->motion.horizontalFov = glm::radians(45.0f);
    }
    SECTION("two quick pauses before the first gameplay sample arrives") {
        context.transition = MatchTransition::Resume;
        ++context.epoch;
        REQUIRE(view.resume(context));
        ++context.epoch;
        REQUIRE(view.resume(context));
        CHECK(view.shown() == nullptr);
        captured->motion.epoch = context.epoch;
    }
    REQUIRE(view.show(*captured));
    CHECK(view.actors().visiblePlayers() == 1);
    CHECK(view.pickupCount() == captured->pickups.size());
    CHECK(view.fixtureCount() == captured->fixtures.size());
    const auto image = CombatPacket::encode(*view.shown());
    const auto textureCount = device.texturesCreated;
    CHECK_FALSE(view.bindCompanions(device, world.powerups(), &weapons));
    CHECK_FALSE(view.loadHud(device, root, nullptr));
    auto invalid = *captured;
    invalid.hud->players[0]->keys = 10;
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->hourglass = HudHourglass{0.25f, 0};
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid)); // No timer archive: do not load from snapshot reception.
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->bossBars.push_back({true, false, {256, 256}});
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid)); // No boss layout was loaded for this scene.
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.players[0]->companions[1]->animation.sequence = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->scroll = HudScroll{4095, 0, -1, 255};
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->help = HudHelp{};
    invalid.hud->help->id = 10; // Not in the native help-message roster.
    REQUIRE(HelpMessages::specOf(10) == nullptr);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.geometry.reset();
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    ++invalid.geometry->layout;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.motion.players[0]->grant = 2;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.players[0]->animation.sequence = 65535;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.motion.tick = 4;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.motion.epoch = 2;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.pickups[0].resource = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(view.pickupCount() == captured->pickups.size());
    invalid = *captured;
    invalid.fixtures[0].resource = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(view.fixtureCount() == captured->fixtures.size());
    invalid = *captured;
    FighterMeshState fighter;
    fighter.incarnation = fighter.part = fighter.resource = 1;
    invalid.fighters.push_back(fighter);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    ProjectileState shot;
    shot.instance = 1;
    shot.continuity = 1;
    shot.resource = 42;
    invalid.projectiles.push_back(shot);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    EnemyCombatState enemy;
    enemy.instance = 1;
    enemy.kind = kGruntKind;
    enemy.health = enemy.fullHealth = 50;
    enemy.animation = {0, 0, 1, 0, 1};
    invalid.enemies.push_back(enemy);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid)); // never load a kind in response to a packet
    CHECK_FALSE(enemies.kindLoaded(kGruntKind));
    CHECK(device.texturesCreated == textureCount);
    REQUIRE(CombatPacket::encode(*view.shown()) == image);

    device.draws.clear();
    view.draw(device, projection, 1920, 1080, 0, nullptr, nullptr, video);
    REQUIRE(device.draws.size() > 2);
    const Mat4 expected =
        motion.camera.clipTransform(video.horizontalFovRadians(), 1920, 1080, projection);
    for (const Vec2 cursor : {Vec2{0.2f, 0.3f}, Vec2{0.8f, 0.7f}}) {
        const auto target = view.cursorAim(0, cursor);
        const auto expectedTarget =
            cursorAimDirection(cursor, expected, motion.players[0]->position);
        REQUIRE(target);
        REQUIRE(expectedTarget);
        CHECK(glm::distance(*target, *expectedTarget) < 0.0001f);
    }
    CHECK_FALSE(view.cursorAim(1, {0.5f, 0.5f}));
    CHECK_FALSE(view.cursorAim(255, {0.5f, 0.5f}));
    CHECK_FALSE(view.cursorAim(0, {-1, 0}));
    // Networking may accept the next sample before the next render. Mouse aiming
    // must use both the camera and the player position from the picture on screen.
    const Vec2 pointer{0.8f, 0.7f};
    const auto displayedAim = view.cursorAim(0, pointer);
    auto pending = *captured;
    pending.motion.players[0]->position += Vec3{15, 0, 20};
    pending.motion.camera.position += Vec3{7, 0, 0};
    REQUIRE(view.show(pending));
    CHECK(view.cursorAim(0, pointer) == displayedAim);
    REQUIRE(view.show(*captured));
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CHECK(device.draws.front().transform[column][row] ==
                  Approx(expected[column][row]).margin(0.0001f));
        }
    }
    CHECK(device.draws.back().state.depthTest);
    const Mat4 hudProjection = makeVirtualScreenTransform(projection, 512, 384, 1920, 1080);
    CHECK(device.draws.back().transform == hudProjection);
    if (video.display.bloom) {
        REQUIRE(device.bloomDrawOffsets.size() == 1);
        REQUIRE(device.ambientOcclusionDrawOffsets.size() == 1);
        REQUIRE(device.depthOfFieldDrawOffsets.size() == 1);
        CHECK(device.ambientOcclusionDrawOffsets[0] < device.bloomDrawOffsets[0]);
        CHECK(device.bloomDrawOffsets[0] == device.depthOfFieldDrawOffsets[0]);
        CHECK(device.depthOfFieldDrawOffsets[0] < device.draws.size());
        const Mat4 clipToView = motion.camera.view() * glm::inverse(expected);
        CHECK(device.ambientOcclusionSettings[0].clipToView == clipToView);
        CHECK(device.depthOfFieldSettings[0].clipToView == clipToView);
        const f32 distance = (motion.camera.view() * Vec4{motion.players[0]->position, 1}).z;
        CHECK(device.depthOfFieldSettings[0].focusEnd == Approx(std::max(20.0f, distance + 15)));
    } else {
        CHECK(device.bloomDrawOffsets.empty());
        CHECK(device.ambientOcclusionDrawOffsets.empty());
        CHECK(device.depthOfFieldDrawOffsets.empty());
    }
    CHECK(CombatPacket::encode(*view.shown()) == image); // Video choices cannot change simulation.
    CHECK(device.texturesCreated == textureCount);
    CHECK(enemies.count() == 0);
    CHECK(enemies.takeBlows().empty());
    auto resumed = context;
    resumed.epoch = captured->motion.epoch + 1;
    resumed.transition = MatchTransition::Resume;
    auto wrongScene = resumed;
    ++wrongScene.scene;
    CHECK_FALSE(view.resume(wrongScene));
    auto wrongGrant = resumed;
    ++wrongGrant.grants[0];
    CHECK_FALSE(view.resume(wrongGrant));
    CHECK_FALSE(view.begin(resumed, world, enemies, resources, pickups, fixtures));
    const auto* figure = view.actors().playerFigure(0);
    REQUIRE(view.resume(resumed));
    CHECK_FALSE(view.show(*captured));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    auto first = *captured;
    first.motion.epoch = resumed.epoch;
    first.motion.tick = 0;
    invalid = first;
    invalid.fixtures[0].resource = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    REQUIRE(view.show(first));
    CHECK(view.actors().playerFigure(0) == figure);
    CHECK(view.actors().visiblePlayers() == 1);
    CHECK(device.texturesCreated == textureCount);
    view.clear();
    CHECK_FALSE(view.cursorAim(0, {0.5f, 0.5f}));
    CHECK(view.shown() == nullptr);
    CHECK(view.actors().visiblePlayers() == 0);
    device.draws.clear();
    view.draw(device, projection, 1920, 1080, 0);
    CHECK(device.draws.size() == 1);
}
} // namespace
