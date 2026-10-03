#include <algorithm>
#include <cmath>
#include <filesystem>
#include <type_traits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/players/ClassData.h"
#include "game/players/NameCheats.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_move_constructible_v<PlayerFigure>);
static_assert(!std::is_copy_constructible_v<PlayerFigure>);

TEST_CASE("green knight drawing interpolates without changing gameplay hand or attack state",
          "[game][figure][cadence][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = 5;
    save.color = 3;
    save.progress().experience = levelExperience(99);
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->familiarTier() == 2);
    bool different = false;
    for (s32 tick = 0; tick < 24; ++tick) {
        figure->animate(1, 1, 1.0f / 60.0f);
        const auto hand = figure->handAttachment(Mat4{1});
        const auto frame = figure->animator().player().frame();
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, nullptr, 0);
        const auto previous = device.draws;
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, nullptr, 0.5f);
        REQUIRE(device.draws.size() == previous.size());
        for (usize draw = 0; draw < previous.size(); ++draw) {
            REQUIRE(device.draws[draw].vertices.size() == previous[draw].vertices.size());
            for (usize vertex = 0; vertex < previous[draw].vertices.size(); ++vertex) {
                different = different || previous[draw].vertices[vertex].position !=
                                             device.draws[draw].vertices[vertex].position;
            }
        }
        CHECK(figure->handAttachment(Mat4{1}) == hand);
        CHECK(figure->animator().player().frame() == frame);
    }
    CHECK(different);
}

TEST_CASE("equipped hand powerups replace the class weapon and restore it when switched off",
          "[game][figure][held-powerup][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::assetOrSkip("POWERUPS/objects.ngc");
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ItemArchive powerups;
    REQUIRE(weapons.load(root / "WEAPONS"));
    REQUIRE(powerups.load(root / "POWERUPS"));
    CharacterSave save;
    save.character = GENERATE(0, 5, 7); // axe, sword, and jester's thrown weapon
    CAPTURE(save.character);
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->heldWeaponBound());
    const Mat4 body = glm::translate(Mat4{1}, Vec3{5, 2, 9});
    const auto hand = figure->handAttachment(body);
    REQUIRE(hand);
    figure->draw(device, Mat4{1}, body, {}, 1, true);
    const usize bodyDraws = device.draws.size();
    device.draws.clear();
    figure->draw(device, Mat4{1}, body, {}, 1, false);
    const auto original = device.draws;
    REQUIRE(original.size() > bodyDraws);

    const auto checkHeld = [&](ItemArchive& archive, std::string_view object) {
        TreeInfo tree;
        TreeNodeInfo node;
        node.name = object;
        node.object = object;
        tree.nodes.push_back(node);
        TreeModel model;
        REQUIRE(model.bind(tree, archive.models, archive.textures, device));
        device.draws.clear();
        model.draw(device, Mat4{1}, *hand, {}, {}, nullptr, 1);
        const auto expected = device.draws;
        REQUIRE_FALSE(expected.empty());
        device.draws.clear();
        figure->draw(device, Mat4{1}, body, {}, 1, false);
        // Exactly the body plus the replacement: never the ordinary weapon alongside it.
        REQUIRE(device.draws.size() == bodyDraws + expected.size());
        for (usize i = 0; i < expected.size(); ++i) {
            const auto& actual = device.draws[bodyDraws + i];
            CHECK(actual.texture == expected[i].texture);
            REQUIRE(actual.vertices.size() == expected[i].vertices.size());
            for (usize v = 0; v < actual.vertices.size(); ++v) {
                CHECK(actual.vertices[v].position == expected[i].vertices[v].position);
            }
        }
    };
    const auto checkRestored = [&] {
        device.draws.clear();
        figure->draw(device, Mat4{1}, body, {}, 1, false);
        REQUIRE(device.draws.size() == original.size());
        for (usize i = bodyDraws; i < original.size(); ++i) {
            CHECK(device.draws[i].texture == original[i].texture);
            REQUIRE(device.draws[i].vertices.size() == original[i].vertices.size());
            CHECK(device.draws[i].vertices.front().position ==
                  original[i].vertices.front().position);
        }
    };

    Inventory inventory;
    inventory.addPowerup(powerup::kWeapon, powerup::kThunderHammer, 1, -1);
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkHeld(weapons, "HAMMER_HD");
    inventory.powerups[0].on = false;
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkRestored();
    inventory.powerups[0].on = true;
    inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 1, -1);
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkHeld(weapons, "SUPERXBOW");
    inventory.addPowerup(powerup::kSpecial, powerup::kRightGauntlet, 0, 1);
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkHeld(powerups, "BOSSGAUNTR");
    inventory.advance(2);
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkHeld(weapons, "SUPERXBOW");
    REQUIRE(inventory.spendPowerup(powerup::kWeapon, powerup::kSuperShot, false));
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkHeld(weapons, "HAMMER_HD");
    REQUIRE(inventory.spendPowerup(powerup::kWeapon, powerup::kThunderHammer, false));
    figure->setWeaponPowerups(device, powerups, weapons, PowerupEffects::of(inventory));
    checkRestored();

    // Missing optional art must not leave the player empty-handed.
    ItemArchive absent;
    PowerupEffects worn;
    worn.weapon = powerup::kThunderHammer;
    figure->setWeaponPowerups(device, powerups, absent, worn);
    checkRestored();
}

TEST_CASE("Phoenix activation follows enabled inventory and expires without requiring art",
          "[game][figure][phoenix]") {
    test::FakeRenderDevice device;
    ItemArchive missing;
    PlayerFigure figure;
    Inventory inventory;
    figure.setCompanionPowerups(device, missing, inventory);
    CHECK_FALSE(figure.phoenixActive());
    inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 1);
    figure.setCompanionPowerups(device, missing, inventory);
    CHECK(figure.phoenixActive());
    inventory.powerups[0].on = false;
    figure.setCompanionPowerups(device, missing, inventory);
    CHECK_FALSE(figure.phoenixActive());
    inventory.advance(2);
    CHECK(inventory.powerups[0].strength == 1);
    inventory.powerups[0].on = true;
    inventory.advance(1);
    figure.setCompanionPowerups(device, missing, inventory);
    CHECK_FALSE(figure.phoenixActive());
    figure.animate(0, 2, 1.0f / 30, PlayerDeed::Die);
    CHECK_FALSE(figure.familiarReleased());
}

TEST_CASE("the warrior's slow swing leaves ghosts of the axe that fade after it",
          "[game][figure][weapon-trail][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    const CharacterSave save;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->heldWeaponBound());
    figure->setMelee(MeleeSense{});
    s32 most = 0;
    bool swung = false;
    for (s32 frame = 0; frame < 90; ++frame) {
        figure->animate(0.0f, 2, 1.0f / 30.0f,
                        frame < 2 ? PlayerDeed::MeleeSlow : PlayerDeed::None);
        swung = swung || figure->animator().action() == PlayerAnimator::Action::SlowSwing;
        figure->updateTrail(Mat4{1.0f}, 2);
        most = std::max(most, static_cast<s32>(figure->trail().count()));
    }
    CHECK(swung);
    CHECK(most > 1);
    CHECK(figure->trail().count() == 0); // gone once the swing is over
    // Drawn, each ghost is the weapon again, fainter.
    device.draws.clear();
    figure->draw(device, Mat4{1.0f}, Mat4{1.0f}, WorldLighting{}, 1.0f, false);
    const usize plain = device.draws.size();
    CHECK(plain > 0);
}

TEST_CASE("X-Ray glasses draw at the posed head only while equipped",
          "[game][figure][xray][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/JES/YEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/JES/ANIM/ANIM.PS2");
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

TEST_CASE("a shield is borne on the second hand, and the jester's hand goes meanwhile",
          "[game][figure][shield][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/JES/YEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/JES/ANIM/ANIM.PS2");
    test::FakeRenderDevice device;
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    CharacterSave save;
    save.character = 7;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    const Mat4 body = glm::translate(Mat4{1}, Vec3{5, 2, 9});
    REQUIRE(figure->armAttachment(body).has_value());
    figure->draw(device, Mat4{1}, body, {}, 1, false);
    const usize bare = device.draws.size();
    device.draws.clear();
    figure->holdOnArm(device, &weapons, "L_SHLD");
    figure->draw(device, Mat4{1}, body, {}, 1, false);
    const auto object = weapons.models.find("L_SHLD");
    REQUIRE(object);
    const auto& mesh = weapons.models.mesh(*object);
    const Mat4 arm = *figure->armAttachment(body);
    CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
        return std::ranges::any_of(mesh.vertices, [&](const auto& vertex) {
            return glm::distance(draw.vertices.front().position,
                                 Vec3{arm * Vec4{vertex.position, 1}}) < 0.0001f;
        });
    }));
    const usize shielded = device.draws.size();
    device.draws.clear();
    figure->holdOnArm(device, &weapons, {});
    figure->draw(device, Mat4{1}, body, {}, 1, false);
    CHECK(device.draws.size() == bare);
    CHECK(shielded != bare); // the shield came, the hand went
}

TEST_CASE("the sign of who is it hangs on the body's root from the realm's items",
          "[game][figure][it][assets]") {
    const auto root =
        test::assetOrSkip("ITEMS/LEVELG/objects.ngc").parent_path().parent_path().parent_path();
    test::assetOrSkip("PLAYERS/JES/YEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/JES/ANIM/ANIM.PS2");
    test::FakeRenderDevice device;
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELG"));
    CharacterSave save;
    save.character = 7;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    const Mat4 body = glm::translate(Mat4{1}, Vec3{5, 2, 9});
    figure->drawMarker(device, items, "IT_SIGN", Mat4{1}, body, {}, 1);
    REQUIRE_FALSE(device.draws.empty());
    const auto object = items.models.find("IT_SIGN");
    REQUIRE(object);
    const auto& mesh = items.models.mesh(*object);
    const Vec3 drawn = device.draws.front().vertices.front().position;
    CHECK(std::ranges::any_of(mesh.vertices, [&](const auto& vertex) {
        return glm::distance(drawn, Vec3{body * Vec4{vertex.position, 1}}) < 0.0001f;
    }));
    device.draws.clear();
    figure->drawMarker(device, items, "NO_SUCH_SIGN", Mat4{1}, body, {}, 1);
    CHECK(device.draws.empty());
}

TEST_CASE("Every headwear powerup names an object of the powerups archive",
          "[game][figure][headwear][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/JES/YEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/JES/ANIM/ANIM.PS2");
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    CharacterSave save;
    save.character = 7;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    const Mat4 body = glm::translate(Mat4{1}, Vec3{5, 2, 9});
    struct Worn {
        u32 special;
        u32 armor;
    };
    for (const Worn wear : {Worn{powerup::kSkorneHorns, 0}, Worn{powerup::kSkorneMask, 0},
                            Worn{0, 0x80000U}, Worn{0, 0x2000U}, Worn{powerup::kXRay, 0}}) {
        CAPTURE(wear.special, wear.armor);
        PowerupEffects worn;
        worn.special = wear.special;
        worn.armor = wear.armor;
        device.draws.clear();
        figure->drawHeadwear(device, powerups, worn, Mat4{1}, body, {}, 1);
        CHECK_FALSE(device.draws.empty());
    }
}

TEST_CASE("Jester throws face the camera and his permanent familiar fires once per release",
          "[game][world][figure][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/JES/SFXGRE/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::assetOrSkip("PLAYERS/JES/GRE/ANIM.PS2");
    test::assetOrSkip("PLAYERS/JES/ANIM/ANIM.PS2");
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

TEST_CASE(
    "Phoenix enables companion release below level thirty and restores the familiar on toggle",
    "[game][figure][phoenix][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    for (const s32 level : {1, 60}) {
        CharacterSave save;
        save.progress().experience = levelExperience(level);
        auto figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        auto& inventory = save.progress().inventory;
        inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 30);
        figure->setCompanionPowerups(device, powerups, inventory);
        REQUIRE(figure->phoenixActive());
        s32 shots = 0;
        bool pending = false;
        for (s32 frame = 0; frame < 120; ++frame) {
            figure->animate(0, 2, 1.0f / 30, PlayerDeed::Attack);
            CHECK(figure->familiarReleased() == pending);
            shots += figure->familiarReleased() ? 1 : 0;
            pending = figure->animator().released();
        }
        CHECK(shots > 1);
        inventory.powerups[0].on = false;
        figure->setCompanionPowerups(device, powerups, inventory);
        CHECK_FALSE(figure->phoenixActive());
        CHECK(figure->familiarTier() == (level < 30 ? 0 : 1));
        // Let the old release retire; subsequent releases require a permanent companion.
        figure->animate(0, 2, 1.0f / 30, PlayerDeed::Attack);
        shots = 0;
        for (s32 frame = 0; frame < 120; ++frame) {
            figure->animate(0, 2, 1.0f / 30, PlayerDeed::Attack);
            shots += figure->familiarReleased() ? 1 : 0;
        }
        CHECK((shots > 0) == (level >= 30));
        inventory.powerups[0].on = true;
        inventory.advance(30);
        figure->setCompanionPowerups(device, powerups, inventory);
        CHECK_FALSE(figure->phoenixActive());
    }
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
    test::convertModelFixture(costume);
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

TEST_CASE("a figure owns shared native texture pixels for its complete draw lifetime",
          "[game][figure][texture-lender]") {
    const auto root = costumeFixture("figure-external", false);
    const auto costume = root / "PLAYERS/WAR/BLU";
    writeTextFile(costume / "textures.json", R"({"bitmaps":[
        {"name":"SKIN","file":"unused.png","width":2,"height":2,"flags":32}]})");
    test::convertModelFixture(costume);
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    REQUIRE_FALSE(PlayerFigure::load(device, root, save, false));
    const auto shared = root / "POWERUPS";
    std::filesystem::create_directories(shared);
    writeFile(shared / "skin.png", test::kTinyPng);
    writeTextFile(shared / "textures.json", R"({"bitmaps":[
        {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(shared);
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->heldWeaponBound());
    for (s32 frame = 0; frame < 3; ++frame) {
        device.draws.clear();
        figure->animate(0, 2, 1.0f / 30);
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false);
        REQUIRE(device.draws.size() == 3);
        for (const auto& draw : device.draws) {
            REQUIRE(draw.texture != &device.whiteTexture());
            const auto* pixels = dynamic_cast<const test::FakeTexture*>(draw.texture);
            REQUIRE(pixels);
            REQUIRE(pixels->pixels.size() == 16);
            CHECK(pixels->pixels[0] == 255);
        }
    }
    device.draws.clear();
    figure.reset();
}

TEST_CASE("native player costumes bind and draw every colour of the sixteen costume classes",
          "[game][figure][texture-lender][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/objects.ngc").parent_path().parent_path();
    // Sumner's separate WIZ/SUM artwork is not a class-colour costume archive.
    for (s32 character = 0; character < kStartingClassCount * 2; ++character) {
        for (s32 color = 0; color < kColorCount; ++color) {
            CAPTURE(character, classCode(character), color);
            test::FakeRenderDevice device;
            CharacterSave save;
            save.character = character;
            save.color = color;
            auto figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(figure);
            figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false);
            CHECK_FALSE(device.draws.empty());
        }
    }
}

TEST_CASE("native name-code costumes bind and animate with their ordinary class",
          "[game][figure][cheats][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/objects.ngc").parent_path().parent_path();
    const s32 level = GENERATE(1, 30, 60, 80);
    for (const auto& costume : hiddenCostumes()) {
        CAPTURE(costume.name, costume.directory, costume.character, level);
        CharacterSave save;
        save.name = costume.name;
        REQUIRE(applyNameCheats(save));
        save.progress().experience = levelExperience(level);
        test::FakeRenderDevice device;
        const auto figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        CHECK(figure->directory().filename() == costume.directory);
        CHECK(figure->animator().bound());
        figure->animate(1, 2, 1.0f / 30.0f);
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false);
        CHECK_FALSE(device.draws.empty());
        CHECK(figure->heldWeaponBound());
        CHECK(figure->missile().bound());
        if (level >= 30) {
            CHECK(figure->familiarTier() == (level >= 80 ? 2 : 1));
            CHECK(figure->familiarMissile().bound());
        }
    }
}

TEST_CASE("missing class throw trees fall back to the costume's first throw tree",
          "[game][figure][cheats]") {
    const auto root = costumeFixture("figure-throw-fallback", false);
    const auto costume = root / "PLAYERS/WAR/BLU";
    writeTextFile(costume / "animations.json", R"({"trees":[
        {"name":"WAR_BLU","nodes":[
            {"name":"HAND","object":"R_WRIST","position":[0,0,0]}],"sequences":[]},
        {"name":"AXE_THROW1","nodes":[
            {"name":"AXE","object":"WEAP_HOLD","position":[0,0,0]}],"sequences":[]}]})");
    test::convertModelFixture(costume);
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    const auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->missile().bound());
    CHECK(figure->missileTree() == "AXE_THROW1");
    REQUIRE(figure->missileArchive());
    CHECK(figure->missileArchive()->trees.find("AXE_THROW1").has_value());
    figure->missile().draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1);
    CHECK_FALSE(device.draws.empty());
}

TEST_CASE("hand replacement priority and restoration render without retail assets",
          "[game][figure][held-powerup]") {
    const auto root = costumeFixture("figure-hand-items", false);
    const auto gear = root / "GEAR";
    std::filesystem::create_directories(gear);
    writeTextFile(gear / "hammer.obj",
                  "v 10 0 0\nv 11 0 0\nv 10 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(gear / "bow.obj",
                  "v 20 0 0\nv 21 0 0\nv 20 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(gear / "gauntlet.obj",
                  "v 30 0 0\nv 31 0 0\nv 30 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeFile(gear / "skin.png", test::kTinyPng);
    writeTextFile(gear / "objects.json", R"({"objects":[
        {"index":0,"name":"HAMMER_HD","file":"hammer.obj","meshTriangles":1},
        {"index":1,"name":"SUPERXBOW","file":"bow.obj","meshTriangles":1},
        {"index":2,"name":"BOSSGAUNTR","file":"gauntlet.obj","meshTriangles":1}]})");
    writeTextFile(gear / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(
        gear / "animations.json",
        R"({"trees":[{"name":"HAMMER","nodes":[{"name":"HAMMER_HD","object":"HAMMER_HD","position":[0,0,0]}]}]})");
    ItemArchive archive;
    test::convertModelFixture(gear);
    REQUIRE(archive.load(gear));
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    const auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    const auto checkWeapon = [&](f32 x, bool hideNormal = false) {
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, hideNormal);
        REQUIRE(device.draws.size() == 3); // two body nodes and exactly one hand weapon
        CHECK(device.draws.back().vertices.front().position == Vec3{x, 2, 3});
    };
    PowerupEffects worn;
    checkWeapon(1);
    worn.weapon = powerup::kThunderHammer;
    figure->setWeaponPowerups(device, archive, archive, worn);
    checkWeapon(11);
    worn.weapon |= powerup::kSuperShot;
    figure->setWeaponPowerups(device, archive, archive, worn);
    checkWeapon(21);
    checkWeapon(21, true); // the held crossbow does not vanish with the thrown class weapon
    worn.special = powerup::kRightGauntlet;
    figure->setWeaponPowerups(device, archive, archive, worn);
    checkWeapon(31);
    worn = {};
    worn.special = powerup::kLeftGauntlet;
    figure->setWeaponPowerups(device, archive, archive, worn);
    checkWeapon(1); // the off-hand gauntlet does not replace the weapon
    worn.special = 0;
    worn.weapon = 1; // elemental overlays keep the ordinary weapon
    figure->setWeaponPowerups(device, archive, archive, worn);
    checkWeapon(1);
    worn.weapon = powerup::kThunderHammer;
    ItemArchive absent;
    figure->setWeaponPowerups(device, absent, absent, worn);
    checkWeapon(1);
}

TEST_CASE("player figures select costume tiers without requiring a scene",
          "[game][world][figure]") {
    const auto root = test::scratchDirectory("figure-costume-tiers");
    CharacterSave save;
    save.color = 1;
    save.progress().experience = levelExperience(25);
    REQUIRE(PlayerFigure::costumeDirectory(root, save).filename() == "BLU");
    std::filesystem::create_directories(root / "PLAYERS/WAR/BLU20");
    writeTextFile(root / "PLAYERS/WAR/BLU20/objects.ngc", "");
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

TEST_CASE("a costume's shadow lies on the ground it is given, apart from the body",
          "[game][world][figure]") {
    const auto root = costumeFixture("figure-shadow", false);
    const auto costume = root / "PLAYERS/WAR/BLU";
    writeTextFile(costume / "objects.json", R"({"objects":[
        {"index":0,"name":"R_WRIST","file":"mesh.obj","meshTriangles":1},
        {"index":1,"name":"ORNAMENT","file":"mesh.obj","meshTriangles":1},
        {"index":2,"name":"WEAP_HOLD","file":"mesh.obj","meshTriangles":1},
        {"index":3,"name":"SHADOWL1","file":"mesh.obj","meshTriangles":1}]})");
    test::FakeRenderDevice device;
    CharacterSave save;
    save.color = 1;
    test::convertModelFixture(costume);
    const auto figure = PlayerFigure::load(device, root, save);
    REQUIRE(figure != nullptr);
    REQUIRE(figure->hasShadow());
    // The body's own draws leave it out.
    figure->draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, false);
    REQUIRE(device.draws.size() == 3);
    device.draws.clear();
    figure->drawShadow(device, Mat4{1.0f}, Vec3{5, 40, 7}, Vec3{5, -2, 7}, Vec3{0, 1, 0}, {}, 0.5f);
    REQUIRE(device.draws.size() == 1);
    const auto& shadow = device.draws[0];
    REQUIRE(shadow.vertices[0].position.x == Catch::Approx(5.0f));
    REQUIRE(shadow.vertices[0].position.y ==
            Catch::Approx(-2.0f + BlobShadow::kLift + BlobShadow::kPull));
    REQUIRE(shadow.vertices[0].position.z == Catch::Approx(7.0f));
    REQUIRE_FALSE(shadow.state.depthWrite);
    // A costume without one draws none.
    const auto plain = PlayerFigure::load(device, costumeFixture("figure-no-shadow", false), save);
    REQUIRE(plain != nullptr);
    REQUIRE_FALSE(plain->hasShadow());
    device.draws.clear();
    plain->drawShadow(device, Mat4{1.0f}, Vec3{0, 40, 0}, Vec3{0.0f}, Vec3{0, 1, 0}, {}, 1.0f);
    REQUIRE(device.draws.empty());
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

TEST_CASE("the body is placed at its scale and lifted by levitation, its shadow's feet apart",
          "[game][world][figure]") {
    const CharacterSave save;
    const Mat4 base = glm::translate(Mat4{1.0f}, Vec3{1.0f, 2.0f, 3.0f});
    const Mat4 plain = PlayerFigure::bodyPlacement(base, save, {});
    CHECK(Vec3{plain[3]} == Vec3{1.0f, 2.0f, 3.0f});
    CHECK(plain[0].x == 1.0f);
    PowerupEffects wings;
    wings.special = powerup::kLevitation | powerup::kGrowth;
    const Mat4 lifted = PlayerFigure::bodyPlacement(base, save, wings);
    CHECK(Vec3{lifted[3]} == Vec3{1.0f, 2.0f + PowerupEffects::kLevitationLift, 3.0f});
    CHECK(lifted[0].x == PowerupEffects::kGrowthScale); // the lift is not scaled
    CHECK(lifted[1].y == PowerupEffects::kGrowthScale);
}
} // namespace
