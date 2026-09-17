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
        "armor": [300, 700], "magic": [100, 500], "height": 5.0, "width": 1.5, "collisionY": 2.5})");
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

} // namespace
