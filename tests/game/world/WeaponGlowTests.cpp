#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/world/WeaponGlow.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path glowArchive() {
    const auto root = test::scratchDirectory("weapon-glow");
    std::filesystem::create_directories(root / "models");
    std::filesystem::create_directories(root / "textures");
    writeTextFile(root / "models/glow.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(root / "objects.json", R"({"objects":[
      {"index":0,"name":"GLOW","file":"models/glow.obj","meshTriangles":1}]})");
    writeFile(root / "textures/skin.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"WEAP_HOLD_RED",
       "nodes":[{"name":"XN","object":"GLOW","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":0,"rate":30}]},
      {"name":"WEAP_TW_R",
       "nodes":[{"name":"XN","object":"GLOW","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":0,"rate":30}]}]})");
    return root;
}

TEST_CASE("an elemental weapon's effects are named by its colour and shown by the hand",
          "[game][world][weapon-glow][damage-types]") {
    CHECK(WeaponGlow::holdTree(1) == "WEAP_HOLD_RED");
    CHECK(WeaponGlow::holdTree(2) == "WEAP_HOLD_BLU");
    CHECK(WeaponGlow::holdTree(3) == "WEAP_HOLD_YEL");
    CHECK(WeaponGlow::holdTree(4) == "WEAP_HOLD_GRE");
    CHECK(WeaponGlow::holdTree(0).empty());
    CHECK(WeaponGlow::throwTree(1) == "WEAP_TW_R");
    CHECK(WeaponGlow::throwTree(2) == "WEAP_TW_B");
    CHECK(WeaponGlow::throwTree(3) == "WEAP_TW_Y");
    CHECK(WeaponGlow::throwTree(4) == "WEAP_TW_G");
    CHECK(WeaponGlow::throwTree(5).empty());
    // The weapon flags' element, unless something else fills the hand.
    PowerupEffects worn;
    worn.weapon = 3 | powerup::kThreeWayShot;
    CHECK(WeaponGlow::elementOf(worn) == 3);
    worn.weapon = 3 | powerup::kSuperShot;
    CHECK(WeaponGlow::elementOf(worn) == 0);
    worn.weapon = 3 | powerup::kThunderHammer;
    CHECK(WeaponGlow::elementOf(worn) == 0);
    worn.weapon = 3;
    worn.special = powerup::kRightGauntlet;
    CHECK(WeaponGlow::elementOf(worn) == 0);
    worn.special = powerup::kLeftGauntlet;
    CHECK(WeaponGlow::elementOf(worn) == 3);
    // The wizards and sorceresses, and the unlockables that shadow them, throw the effect alone.
    CHECK(WeaponGlow::throwsEffectAlone(2));
    CHECK(WeaponGlow::throwsEffectAlone(6));
    CHECK(WeaponGlow::throwsEffectAlone(10));
    CHECK(WeaponGlow::throwsEffectAlone(14));
    CHECK_FALSE(WeaponGlow::throwsEffectAlone(0));
    CHECK_FALSE(WeaponGlow::throwsEffectAlone(3));
    CHECK(WeaponGlow::tierOf(1) == 0);
    CHECK(WeaponGlow::tierOf(9) == 0);
    CHECK(WeaponGlow::tierOf(10) == 1);
    CHECK(WeaponGlow::tierOf(99) == 9);
    CHECK(WeaponGlow::tierOf(120) == 9);
}

TEST_CASE("the glow is kept in the hand while its element is worn and put out after",
          "[game][world][weapon-glow][damage-types]") {
    const auto root = glowArchive();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    EffectTrees effects;
    WeaponGlow glow;
    const Mat4 hand = glm::translate(Mat4{1.0f}, Vec3{1.0f, 2.0f, 3.0f});
    glow.update(device, effects, &archive, 1, hand, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.5f});
    REQUIRE(effects.count() == 1);
    REQUIRE(glow.effect() != 0);
    CHECK(glow.element() == 1);
    CHECK(effects.effect(0).position == Vec3{1.0f, 3.0f, 3.0f});
    const u32 first = glow.effect();
    // It lasts as long as it is worn, following the hand, one effect for the whole time.
    effects.update(5.0f);
    glow.update(device, effects, &archive, 1, glm::translate(hand, Vec3{4.0f, 0.0f, 0.0f}),
                Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f});
    REQUIRE(effects.count() == 1);
    CHECK(glow.effect() == first);
    CHECK(effects.effect(0).position == Vec3{5.0f, 3.0f, 3.0f});
    // Another element the archive lacks shows nothing; the first comes back new.
    glow.update(device, effects, &archive, 2, hand, Vec3{0.0f}, Vec3{0.0f});
    CHECK(effects.count() == 0);
    CHECK(glow.effect() == 0);
    CHECK(glow.element() == 2);
    glow.update(device, effects, &archive, 1, hand, Vec3{0.0f}, Vec3{0.0f});
    REQUIRE(effects.count() == 1);
    CHECK(glow.effect() != first);
    // No element, or no archive, puts it out.
    glow.update(device, effects, nullptr, 1, hand, Vec3{0.0f}, Vec3{0.0f});
    CHECK(effects.count() == 1);
    glow.update(device, effects, &archive, 0, hand, Vec3{0.0f}, Vec3{0.0f});
    CHECK(effects.count() == 0);
    CHECK(glow.element() == 0);
    glow.clear(effects);
    CHECK(effects.count() == 0);
}

TEST_CASE("the classes' effects hold every element's glow and throw effect",
          "[game][world][weapon-glow][damage-types][unpacked]") {
    const auto path = test::unpackedOrSkip("PLAYERS/WAR/SFXYEL/animations.json").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(path));
    EffectTrees effects;
    WeaponGlow glow;
    for (u32 element = 1; element <= 4; ++element) {
        CAPTURE(element);
        REQUIRE(archive.trees.find(WeaponGlow::holdTree(element)).has_value());
        REQUIRE(archive.trees.find(WeaponGlow::throwTree(element)).has_value());
        glow.update(device, effects, &archive, element, Mat4{1.0f}, Vec3{0.0f, 0.5f, 0.0f},
                    Vec3{0.8f});
        REQUIRE(effects.count() == 1);
        effects.update(1.0f);
        effects.draw(device, Mat4{1.0f}, {});
        CHECK(effects.count() == 1);
    }
    glow.clear(effects);
    CHECK(effects.count() == 0);
}
} // namespace
