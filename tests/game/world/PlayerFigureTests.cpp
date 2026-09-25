#include <algorithm>
#include <cmath>
#include <filesystem>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/Progression.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_move_constructible_v<PlayerFigure>);
static_assert(!std::is_copy_constructible_v<PlayerFigure>);

TEST_CASE("X-Ray glasses draw at the posed head only while equipped",
          "[game][figure][xray][unpacked]") {
    const auto root = test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("PLAYERS/JES/YEL/animations.json");
    test::unpackedOrSkip("PLAYERS/JES/ANIM/animations.json");
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    CharacterSave save;
    save.character = 7;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    const Mat4 body = glm::translate(Mat4{1}, Vec3{5, 2, 9});
    PowerupEffects worn;
    figure->drawHeadwear(device, powerups, worn, Mat4{1}, body, {}, 1);
    CHECK(device.draws.empty());
    worn.special = powerup::kXRay;
    figure->drawHeadwear(device, powerups, worn, Mat4{1}, body, {}, 1);
    REQUIRE_FALSE(device.draws.empty());
    const auto head = figure->attachment(body, "HEAD");
    REQUIRE(head);
    const auto object = powerups.models.find("HEAD_XRAY");
    REQUIRE(object);
    const auto& mesh = powerups.models.mesh(*object);
    const Vec3 drawn = device.draws.front().vertices.front().position;
    CHECK(std::ranges::any_of(mesh.vertices, [&](const auto& vertex) {
        return glm::distance(drawn, Vec3{*head * Vec4{vertex.position, 1}}) < 0.0001f;
    }));
    device.draws.clear();
    worn.special = 0;
    figure->drawHeadwear(device, powerups, worn, Mat4{1}, body, {}, 1);
    CHECK(device.draws.empty());
}

TEST_CASE("Jester throws face the camera and his permanent familiar fires once per release",
          "[game][world][figure][unpacked]") {
    const auto root = test::unpackedOrSkip("PLAYERS/JES/SFXGRE/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("PLAYERS/JES/GRE/animations.json");
    test::unpackedOrSkip("PLAYERS/JES/ANIM/animations.json");
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = 7;
    save.color = 3;
    save.progress().experience = levelExperience(60);
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure != nullptr);
    REQUIRE(figure->missile().bound());
    REQUIRE(figure->familiarTier() == 1);
    REQUIRE(figure->familiarMissile().bound());
    CameraFrame camera;
    camera.right = {0, 0, -1};
    camera.forward = {-1, 0, 0};
    PlayerMissiles missiles;
    missiles.bindVisuals(device);
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(7);
    launch.model = &figure->missile();
    launch.archive = figure->missileArchive();
    launch.tree = figure->missileTree();
    launch.velocity = Vec3{0, 0, 35};
    REQUIRE(missiles.launch(launch));
    missiles.draw(device, Mat4{1}, {}, &camera);
    REQUIRE_FALSE(device.draws.empty());
    bool hasArea = false;
    for (const auto& draw : device.draws) {
        for (usize i = 0; i + 2 < draw.vertices.size(); i += 3) {
            const Vec3 a = draw.vertices[i + 1].position - draw.vertices[i].position;
            const Vec3 b = draw.vertices[i + 2].position - draw.vertices[i].position;
            hasArea |= std::abs(glm::dot(glm::cross(a, b), camera.forward)) > 0.001f;
        }
    }
    CHECK(hasArea); // The billboard is not edge-on to this side-view camera.
    bool pending = false;
    s32 releases = 0;
    s32 familiarShots = 0;
    for (s32 frame = 0; frame < 180; ++frame) {
        figure->animate(0, 2, 1.0f / 30, PlayerDeed::Attack);
        CHECK(figure->familiarReleased() == pending);
        familiarShots += figure->familiarReleased() ? 1 : 0;
        pending = figure->animator().released();
        releases += pending ? 1 : 0;
    }
    CHECK(releases > 1);
    CHECK(familiarShots == releases - (pending ? 1 : 0));

    ClassDataSet classes;
    REQUIRE(classes.load(root / "pdata"));
    const auto* stats = classes.stats(save.character);
    REQUIRE(stats != nullptr);
    REQUIRE(stats->familiarShotOffset.y > 0); // requires refreshed PDAT export
    PlayerActor actor;
    actor.spawn(0, save, stats, Vec3{0}, 0);
    ItemArchive weapons;
    const WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerArsenal arsenal;
    arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
    arsenal.launchFamiliar(actor, figure.get(), Vec3{0, 5, 20});
    REQUIRE(arsenal.missiles().count() == 1);
    CHECK(arsenal.missiles().missile(0).damage == 6);
    CHECK(arsenal.missiles().missile(0).position == stats->familiarShotOffset);
    CHECK(arsenal.missiles().missile(0).spec->radius == 1);
    CHECK(arsenal.missiles().missile(0).spec->weight == 10);
}

TEST_CASE("player figure scale prioritizes ogre and growth over mastery", "[game][world][figure]") {
    CharacterSave save;
    REQUIRE(PlayerFigure::bodyScale(save, {}) == 1.0f);
    save.progress().experience = levelExperience(kMaxLevel);
    REQUIRE(PlayerFigure::bodyScale(save, {}) == 1.2f);
    PowerupEffects growth;
    growth.special = powerup::kGrowth;
    REQUIRE(PlayerFigure::bodyScale(save, growth) == PowerupEffects::kGrowthScale);
    save.character = 12;
    REQUIRE(PlayerFigure::bodyScale(save, growth) == 1.6f);
    REQUIRE(PlayerFigure::bodyScale(save, {}) == 1.6f);
}

/** A costume with a wrist and an unmapped ornament, sharing a tiny synthetic mesh. */
std::filesystem::path costumeFixture(std::string_view name, bool animated) {
    const auto root = test::scratchDirectory(name);
    const auto costume = root / "PLAYERS/WAR/BLU";
    std::filesystem::create_directories(costume);
    writeTextFile(costume / "mesh.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(costume / "skin.png", test::kTinyPng);
    writeTextFile(costume / "objects.json", R"({"objects":[
        {"index":0,"name":"R_WRIST","file":"mesh.obj","meshTriangles":1},
        {"index":1,"name":"ORNAMENT","file":"mesh.obj","meshTriangles":1},
        {"index":2,"name":"WEAP_HOLD","file":"mesh.obj","meshTriangles":1}]})");
    writeTextFile(costume / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(costume / "animations.json", R"({"trees":[
        {"name":"WAR_BLU","nodes":[
            {"name":"HAND","object":"R_WRIST","position":[1,2,3]},
            {"name":"ORNAMENT","object":"ORNAMENT","position":[4,0,0]}],"sequences":[]}]})");
    if (animated) {
        const auto actions = root / "PLAYERS/WAR/ANIM";
        std::filesystem::create_directories(actions);
        // HAND is deliberately at a different index; ORNAMENT has no class counterpart.
        writeTextFile(actions / "animations.json", R"({"trees":[
            {"name":"WAR","nodes":[
                {"name":"UNUSED","position":[0,0,0]},
                {"name":"HAND","position":[7,8,9]}],
             "sequences":[{"name":"READY","frames":60,"frameRate":30,"repeats":true}]}]})");
    }
    return root;
}

TEST_CASE("an unloaded player figure is safe to animate and draw", "[game][world][figure]") {
    test::FakeRenderDevice device;
    PlayerFigure figure;
    REQUIRE_FALSE(figure.heldWeaponBound());
    REQUIRE_FALSE(figure.animator().bound());
    REQUIRE_FALSE(figure.missile().bound());
    REQUIRE_FALSE(figure.throwSound().has_value());
    REQUIRE(figure.effects() == nullptr);
    REQUIRE_FALSE(figure.handPosition(Mat4{1.0f}).has_value());
    figure.animate(1.0f, 2, 1.0f / 30.0f);
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, false);
    REQUIRE(device.draws.empty());
    REQUIRE(PlayerFigure::load(device, test::scratchDirectory("figure-missing"), CharacterSave{}) ==
            nullptr);
}

TEST_CASE("player figures select costume tiers without requiring a scene",
          "[game][world][figure]") {
    const auto root = test::scratchDirectory("figure-costume-tiers");
    CharacterSave save;
    save.color = 1;
    save.progress().experience = levelExperience(25);
    REQUIRE(PlayerFigure::costumeDirectory(root, save).filename() == "BLU");
    std::filesystem::create_directories(root / "PLAYERS/WAR/BLU20");
    writeTextFile(root / "PLAYERS/WAR/BLU20/objects.json", "{}");
    REQUIRE(PlayerFigure::costumeDirectory(root, save).filename() == "BLU20");
}

TEST_CASE("a player figure draws a rest-pose weapon when optional animations are missing",
          "[game][world][figure]") {
    const auto root = costumeFixture("figure-rest-pose", false);
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    const auto figure = PlayerFigure::load(device, root, save);
    REQUIRE(figure != nullptr);
    REQUIRE(figure->directory() == root / "PLAYERS/WAR/BLU");
    REQUIRE(figure->heldWeaponBound());
    REQUIRE_FALSE(figure->animator().bound());
    REQUIRE_FALSE(figure->handPosition(Mat4{1.0f}).has_value());
    figure->animate(1.0f, 2, 1.0f / 30.0f);
    figure->draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, false);
    REQUIRE(device.draws.size() == 3);
    REQUIRE(device.draws.back().vertices[0].position == Vec3{1.0f, 2.0f, 3.0f});
    device.draws.clear();
    figure->draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, true);
    REQUIRE(device.draws.size() == 2);
}

TEST_CASE("a player figure maps animation nodes by name and poses its held weapon",
          "[game][world][figure]") {
    const auto root = costumeFixture("figure-posed", true);
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    const auto figure = PlayerFigure::load(device, root, save);
    REQUIRE(figure != nullptr);
    REQUIRE(figure->animator().bound());
    const Mat4 body = glm::scale(glm::translate(Mat4{1.0f}, Vec3{10.0f, 20.0f, 30.0f}), Vec3{2.0f});
    const auto hand = figure->handPosition(body);
    REQUIRE(hand.has_value());
    REQUIRE(*hand == Vec3{24.0f, 36.0f, 48.0f});
    figure->animate(0.0f, 2, 1.0f / 30.0f);
    figure->draw(device, Mat4{1.0f}, body, {}, 0.5f, false);
    REQUIRE(device.draws.size() == 3);
    REQUIRE(device.draws[0].vertices[0].position == *hand);
    REQUIRE(device.draws[1].vertices[0].position == Vec3{18.0f, 20.0f, 30.0f});
    REQUIRE(device.draws[2].vertices[0].position == *hand);
    for (const auto& draw : device.draws) {
        REQUIRE(draw.state.blend == BlendMode::Alpha);
        REQUIRE_FALSE(draw.state.depthWrite);
    }
}
} // namespace
