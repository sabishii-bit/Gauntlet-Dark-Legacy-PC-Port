#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/players/ClassData.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("class and colour codes name the assets", "[game][players]") {
    REQUIRE(classCode(0) == "WAR");
    REQUIRE(classCode(7) == "JES");
    REQUIRE(classCode(kSumnerClass) == "SUM");
    REQUIRE(classCode(kClassCount).empty());
    REQUIRE(colorCode(0) == "YEL");
    REQUIRE(colorCode(2) == "RED");
    REQUIRE(colorCode(4).empty());
    REQUIRE(playerColor(2) == Color::rgba(255, 192, 224));
    REQUIRE(playerColor(9) == Color::white());
}

TEST_CASE("starting classes are always unlocked and the rest by their bit", "[game][players]") {
    REQUIRE(classUnlocked(0, 0));
    REQUIRE(classUnlocked(7, 0));
    REQUIRE_FALSE(classUnlocked(8, 0));
    REQUIRE(classUnlocked(8, 1));
    REQUIRE_FALSE(classUnlocked(kSumnerClass, 0xFF));
    REQUIRE(classUnlocked(kSumnerClass, 0x100));
    REQUIRE_FALSE(classUnlocked(-1, 0xFFFF));
}

TEST_CASE("class stats load from their files", "[game][players]") {
    const auto dir = test::scratchDirectory("class-data");
    writeTextFile(dir / "WAR.json", R"({"code": "WAR", "fight": [600, 999], "speed": [350, 750],
        "armor": [300, 700], "magic": [100, 500], "height": 5.0, "width": 1.5, "collisionY": 2.5,
        "weaponOffset": [-0.5, 0.5, 1.5]})");
    writeTextFile(dir / "WIZ.json", R"({"fight": [250, 650], "speed": [350, 750],
        "armor": [150, 550], "magic": [600, 999]})");
    writeTextFile(dir / "VAL.json", "{broken");
    ClassDataSet classes;
    REQUIRE(classes.load(dir));
    REQUIRE(classes.loadedCount() == 2);
    REQUIRE(classes.stats(0) != nullptr);
    REQUIRE(classes.stats(0)->fightMin == 600.0f);
    REQUIRE(classes.stats(0)->height == 5.0f);
    REQUIRE(classes.stats(0)->collisionY == 2.5f);
    REQUIRE(classes.stats(0)->weaponOffset == Vec3{-0.5f, 0.5f, 1.5f});
    REQUIRE(classes.stats(2)->weaponOffset == Vec3{0.0f, 0.0f, 0.0f}); // a file without one
    REQUIRE(classes.stats(2)->magicMax == 999.0f);
    REQUIRE(classes.stats(1) == nullptr);
    REQUIRE(classes.stats(kSumnerClass) == nullptr);
    REQUIRE(classes.stats(-1) == nullptr);

    ClassDataSet empty;
    REQUIRE_FALSE(empty.load(test::scratchDirectory("class-data-empty")));
    REQUIRE_FALSE(empty.loaded());
}

TEST_CASE("the unpacked class data covers the sixteen playable classes",
          "[game][players][unpacked]") {
    const std::filesystem::path dir = test::unpackedOrSkip("pdata/WAR.json").parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(dir));
    REQUIRE(classes.loadedCount() == 16);
    REQUIRE(classes.stats(0)->fightMin == 600.0f);
    REQUIRE(classes.stats(2)->magicMin == 600.0f);
}

TEST_CASE("status boxes are tinted by costume, dimmer when nobody joined", "[game][players]") {
    REQUIRE(boxTint(0, true) == Color::rgba(240, 240, 0));
    REQUIRE(boxTint(0, false) == Color::rgba(180, 180, 60));
    REQUIRE(boxTint(3, true) == Color::rgba(0, 200, 0));
    REQUIRE(boxTint(7, true) == Color::white());
}

TEST_CASE("a class's moves load with its stats, each a chain of strikes", "[game][players]") {
    const auto dir = test::scratchDirectory("class-moves");
    writeTextFile(dir / "WAR.json", R"({"fight": [600, 999], "speed": [350, 750],
  "armor": [300, 700], "magic": [100, 500], "height": 5, "width": 1.5,
  "moves": {"turboAThrow": 2, "turboB": 0, "turboC1": 1, "turboC2": 2, "combo1": -1},
  "moveEffects": [
    {"next": 1, "tree": "WAR_POWERB", "sound": "S_WARTURBOB", "offset": [0, 5, 0], "scale": 2},
    {"next": -1, "tree": "NULLFX", "sound": ""}],
  "moveStrikes": [
    {"type": 4, "radius": 12, "delay": 0.5, "arc": -1, "amount": 50, "effect": 0, "next": -1,
     "hitEffect": 1, "damageType": 257, "flags": 16, "help": 57},
    {"type": 4, "radius": 8, "delay": 0.5, "arc": 0.5, "amount": -2, "effect": 1, "next": 2},
    {"type": 2, "hitRadius": 10, "maxTime": 6, "offset": [0, 1, 5], "amount": 70,
     "speedMin": 30, "speedMax": 40, "effect": -1, "next": 1, "startFrame": 9}]})");
    ClassDataSet classes;
    REQUIRE(classes.load(dir));
    const ClassStats* war = classes.stats(0);
    REQUIRE(war != nullptr);
    REQUIRE(war->moves.turboB == 0);
    REQUIRE(war->moves.turboAThrow == 2);
    REQUIRE(war->moveStrikes[0].hitEffect == 1);
    REQUIRE(war->moveStrikes[0].damageType == 257U);
    REQUIRE(war->moveStrikes[0].help == 57);
    REQUIRE(war->moveStrikes[0].dimming() == -0.4f);
    REQUIRE(war->moveStrikes[0].harms());
    MoveStrike span;
    span.type = MoveStrike::kWindow;
    span.flags = MoveStrike::kHidesWeapon;
    span.startFrame = 5;
    span.endFrame = 40;
    REQUIRE_FALSE(span.harms());
    REQUIRE_FALSE(span.lasting(4.9f));
    REQUIRE(span.lasting(5.0f));
    REQUIRE_FALSE(span.lasting(40.0f));
    REQUIRE(war->moves.turboC2 == 2);
    REQUIRE(war->moves.combo1 == -1);
    REQUIRE(war->moveEffects.size() == 2);
    REQUIRE(war->moveEffects[0].tree == "WAR_POWERB");
    REQUIRE(war->moveEffects[0].next == 1);
    REQUIRE(war->moveEffects[0].offset == Vec3{0.0f, 5.0f, 0.0f});
    REQUIRE(war->moveStrikes.size() == 3);
    REQUIRE(war->moveStrikes[2].type == MoveStrike::kFlies);
    REQUIRE(war->moveStrikes[2].speed == 35.0f); // half way between its least and its most
    REQUIRE(war->moveStrikes[2].startFrame == 9);
    REQUIRE(war->strikesOf(0) == std::vector<int>{0});
    REQUIRE(war->strikesOf(1) == std::vector<int>{1, 2, 1}); // a ring is followed once round
    REQUIRE(war->strikesOf(-1).empty());
    REQUIRE(war->strikesOf(9).empty());
}

} // namespace
