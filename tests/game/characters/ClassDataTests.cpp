#include <array>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/characters/ClassData.h"

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

TEST_CASE("every class's name is its index, in the order of the asset codes",
          "[game][players][class-names]") {
    struct NamedClass {
        s32 constant;
        s32 index;
        std::string_view code;
    };
    constexpr std::array<NamedClass, kClassCount> kNamed{{
        {kWarriorClass, 0, "WAR"},
        {kValkyrieClass, 1, "VAL"},
        {kWizardClass, 2, "WIZ"},
        {kArcherClass, 3, "ARC"},
        {kDwarfClass, 4, "DWF"},
        {kKnightClass, 5, "KNI"},
        {kSorceressClass, 6, "SOR"},
        {kJesterClass, 7, "JES"},
        {kMinotaurClass, 8, "MIN"},
        {kFalconessClass, 9, "FAL"},
        {kJackalClass, 10, "JAC"},
        {kTigressClass, 11, "TIG"},
        {kOgreClass, 12, "OGR"},
        {kUnicornClass, 13, "UNI"},
        {kMedusaClass, 14, "MED"},
        {kHyenaClass, 15, "HYE"},
        {kSumnerClass, 16, "SUM"},
    }};
    for (const auto& [constant, index, code] : kNamed) {
        CAPTURE(index);
        CHECK(constant == index);
        CHECK(classCode(constant) == code);
    }
    CHECK(baseClassOf(kMinotaurClass) == kWarriorClass);
    CHECK(baseClassOf(kFalconessClass) == kValkyrieClass);
    CHECK(baseClassOf(kJackalClass) == kWizardClass);
    CHECK(baseClassOf(kTigressClass) == kArcherClass);
    CHECK(baseClassOf(kOgreClass) == kDwarfClass);
    CHECK(baseClassOf(kUnicornClass) == kKnightClass);
    CHECK(baseClassOf(kMedusaClass) == kSorceressClass);
    CHECK(baseClassOf(kHyenaClass) == kJesterClass);
}

TEST_CASE("a character plays as its base class and wears its own archive, Sumner the wizard's",
          "[game][players]") {
    REQUIRE(baseClassOf(0) == 0);
    REQUIRE(baseClassOf(7) == 7);
    REQUIRE(baseClassOf(9) == 1);  // the falconess shadows the valkyrie
    REQUIRE(baseClassOf(15) == 7); // the hyena the jester
    REQUIRE(baseClassOf(kSumnerClass) == kWizardClass);
    REQUIRE(baseClassOf(-1) == -1);
    REQUIRE(baseClassOf(kClassCount) == kClassCount);
    REQUIRE(archiveClassOf(9) == 9);
    REQUIRE(archiveClassOf(kSumnerClass) == kWizardClass);
    REQUIRE(kSumnerCostume == "SUM");
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
        "armor": [300, 700], "magic": [100, 500], "height": 5.0, "width": 1.5,
        "attachY": 4.4, "collisionY": 2.5,
        "weaponOffset": [-0.5, 0.5, 1.5], "streakForward": 2.5,
        "weaponGlowOffsets": [[0, -0.35, 0.75], [0, -0.2, 0.55]],
        "weaponGlowScales": [[0.45, 0.55, 0.55], [0.55, 0.75, 0.7], [1, 2]]})");
    writeTextFile(dir / "WIZ.json", R"({"fight": [250, 650], "speed": [350, 750],
        "armor": [150, 550], "magic": [600, 999]})");
    writeTextFile(dir / "VAL.json", "{broken");
    ClassDataSet classes;
    REQUIRE(classes.load(dir));
    REQUIRE(classes.loadedCount() == 2);
    REQUIRE(classes.stats(0) != nullptr);
    REQUIRE(classes.stats(0)->fightMin == 600.0f);
    REQUIRE(classes.stats(0)->height == 5.0f);
    CHECK(classes.stats(0)->streakForward == 2.5f);
    CHECK(classes.stats(2)->streakForward == 0.0f);
    REQUIRE(classes.stats(0)->collisionY == 2.5f);
    REQUIRE(classes.stats(0)->attentionY == 4.4f);
    REQUIRE(classes.stats(0)->weaponOffset == Vec3{-0.5f, 0.5f, 1.5f});
    REQUIRE(classes.stats(2)->weaponOffset == Vec3{0.0f, 0.0f, 0.0f}); // a file without one
    REQUIRE(classes.stats(0)->weaponGlowOffsets[0] == Vec3{0.0f, -0.35f, 0.75f});
    REQUIRE(classes.stats(0)->weaponGlowOffsets[1] == Vec3{0.0f, -0.2f, 0.55f});
    REQUIRE(classes.stats(0)->weaponGlowOffsets[2] == Vec3{0.0f});
    REQUIRE(classes.stats(0)->weaponGlowScales[1] == Vec3{0.55f, 0.75f, 0.7f});
    REQUIRE(classes.stats(0)->weaponGlowScales[2] == Vec3{0.0f}); // not three values
    REQUIRE(classes.stats(2)->weaponGlowScales[0] == Vec3{0.0f});
    REQUIRE(classes.stats(2)->magicMax == 999.0f);
    REQUIRE(classes.stats(1) == nullptr);
    // Sumner plays on the wizard's record: there is no seventeenth file.
    REQUIRE(classes.stats(kSumnerClass) == classes.stats(2));
    REQUIRE(classes.stats(kSumnerClass) != nullptr);
    REQUIRE(classes.stats(-1) == nullptr);

    ClassDataSet empty;
    REQUIRE_FALSE(empty.load(test::scratchDirectory("class-data-empty")));
    REQUIRE_FALSE(empty.loaded());
}

TEST_CASE("the native class data covers the sixteen playable classes", "[game][players][assets]") {
    const std::filesystem::path dir = test::assetOrSkip("PDATA/WAR.WAD").parent_path();
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
  "moves": {"turboAClose": 0, "turboALow": 1, "turboAStep": 2, "turboA360": 1,
            "turboAThrow": 2, "turboB": 0, "turboC1": 1, "turboC2": 2, "combo1": -1},
  "moveEffects": [
    {"next": 1, "tree": "WAR_POWERB", "sound": "S_WARTURBOB", "offset": [0, 5, 0], "scale": 2},
    {"next": -1, "tree": "NULLFX", "sound": "", "color": 4278255360},
    {"next": -1, "tree": "WIZ_HEAD_Y", "sound": "R_WRIST", "flags": 33554432}],
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
    REQUIRE(war->moves.turboAClose == 0);
    REQUIRE(war->moves.turboALow == 1);
    REQUIRE(war->moves.turboAStep == 2);
    REQUIRE(war->moves.turboA360 == 1);
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
    REQUIRE(war->moveEffects.size() == 3);
    REQUIRE(war->moveEffects[0].tree == "WAR_POWERB");
    CHECK(war->moveEffects[0].color == 0xFFFFFFFF); // legacy exports omit the field
    CHECK(war->moveEffects[0].tint() == Color::white());
    CHECK(war->moveEffects[1].color == 0xFF00FF00);
    CHECK(war->moveEffects[1].tint() == Color::rgba(0, 255, 0));
    REQUIRE_FALSE(war->moveEffects[0].particle());
    REQUIRE(war->moveEffects[2].particle()); // a hand glow: texture and node, not tree and sound
    REQUIRE(war->moveEffects[0].next == 1);
    REQUIRE(war->moveEffects[0].offset == Vec3{0.0f, 5.0f, 0.0f});
    REQUIRE(war->moveStrikes.size() == 3);
    REQUIRE(war->moveStrikes[2].type == MoveStrike::kFlies);
    REQUIRE(war->moveStrikes[2].speed == 35.0f); // half way between its least and its most
    REQUIRE(war->moveStrikes[2].startFrame == 9);
    REQUIRE(war->strikesOf(0) == std::vector<s32>{0});
    REQUIRE(war->strikesOf(1) == std::vector<s32>{1, 2, 1}); // a ring is followed once round
    REQUIRE(war->strikesOf(-1).empty());
    REQUIRE(war->strikesOf(9).empty());
}

TEST_CASE("move effect tint decodes RGB without treating its high byte as opacity",
          "[game][players][alpha-kiss-color]") {
    MoveEffect effect;
    CHECK(effect.tint() == Color::white()); // native FFFFFFFF sentinel
    for (const u32 packed : {0x0012AB34U, 0x7F12AB34U, 0xFF12AB34U}) {
        effect.color = packed;
        CHECK(effect.tint() == Color::rgba(0x12, 0xAB, 0x34, 255));
    }
    effect.color = 0x7FFFFFFF; // common native explicit white, not the sentinel
    CHECK(effect.tint() == Color::white());
    effect.color = 0;
    CHECK(effect.tint() == Color::rgba(0, 0, 0, 255));
}

TEST_CASE("a beast plays on its own record, fifty over its base class on every base stat",
          "[game][players][assets]") {
    const auto pdata = test::unpackedOrSkip("PDATA/MIN.json").parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(pdata));
    for (s32 base = 0; base < kStartingClassCount; ++base) {
        const s32 beast = base + kStartingClassCount;
        CAPTURE(base, beast);
        const ClassStats* baseStats = classes.stats(base);
        const ClassStats* beastStats = classes.stats(beast);
        REQUIRE(baseStats != nullptr);
        REQUIRE(beastStats != nullptr);
        CHECK(beastStats != baseStats);
        CHECK(beastStats->fightMin == baseStats->fightMin + 50.0f);
        CHECK(beastStats->speedMin == baseStats->speedMin + 50.0f);
        CHECK(beastStats->armorMin == baseStats->armorMin + 50.0f);
        CHECK(beastStats->magicMin == baseStats->magicMin + 50.0f);
    }
    // Sumner is the wizard's record still.
    CHECK(classes.stats(kSumnerClass) == classes.stats(kWizardClass));
}

TEST_CASE("a beast whose record is missing plays on its base class's", "[game][players]") {
    const auto directory = test::scratchDirectory("class-data-beast-fallback");
    writeTextFile(directory / "WAR.json", R"({"code":"WAR","fight":[600,999],"speed":[350,750],
        "armor":[300,700],"magic":[100,500],"height":5,"width":1.5,"attachY":4,"collisionY":2.5,
        "powerupTime":1})");
    ClassDataSet classes;
    REQUIRE(classes.load(directory));
    REQUIRE(classes.stats(0) != nullptr);
    CHECK(classes.stats(kStartingClassCount) == classes.stats(0)); // the minotaur, the warrior's
    CHECK(classes.stats(kStartingClassCount + 1) == nullptr);      // the falconess has no valkyrie
}

} // namespace
