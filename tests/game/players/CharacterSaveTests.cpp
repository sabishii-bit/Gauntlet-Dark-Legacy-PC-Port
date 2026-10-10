#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/players/CharacterSave.h"

namespace {

using namespace gdl;
using namespace gdl::game;

CharacterSave sampleSave() {
    CharacterSave save;
    save.name = "PELE";
    save.character = 2;
    save.color = 3;
    save.classUnlock = 0x101;
    save.gold = 1500;
    save.levelTotal = 7;
    save.classes[2].experience = 1060;
    save.classes[2].fightAdd = 12.5f;
    save.classes[9].health = 320;
    save.classes[2].crystals[1] = 15;
    save.classes[2].unlocked = 0b10;
    save.classes[2].inventory.keys = 4;
    save.classes[2].inventory.potions = {1, 3, 3};
    save.classes[2].inventory.addPowerup(9, 0x8000, 2.0f, 45.0f);
    save.classes[2].inventory.addPowerup(5, 0x80000, 0.0f, 30.0f);
    save.classes[2].inventory.powerups[1].on = false; // taken off in the selector
    save.classes[2].levels.recordBeaten(7, 0, 8, 0);
    save.classes[2].levels.recordBeaten(7, 0, 8, 0);
    save.classes[2].levels.recordBeaten(7, 2, 0, 9);
    save.classes[2].levels.recordBossDeath(2);
    return save;
}

TEST_CASE("autoattack defaults on for new and legacy characters",
          "[game][players][save][autoattack]") {
    // InitPlayerControls enables it; old remake saves have no preference to restore.
    CHECK(CharacterSave{}.autoAttack);
    CHECK(CharacterSave::fromJson(R"({"name":"OLD","character":0})").autoAttack);
    CHECK(CharacterSave::fromJson(R"({"version":1,"name":"OLD","character":0})").autoAttack);
    CHECK_FALSE(
        CharacterSave::fromJson(R"({"version":1,"name":"OFF","character":0,"autoAttack":false})")
            .autoAttack);
    CHECK(CharacterSave::fromJson(R"({"version":1,"name":"ON","character":0,"autoAttack":true})")
              .autoAttack);
}

TEST_CASE("autoattack persists with the named character across class switches",
          "[game][players][save][autoattack]") {
    // player_get_from_save/player_store_in_save use the shared control_autoattack
    // header, not P_SAVE_STUFF[character]. A different name keeps its own setting.
    auto save = sampleSave();
    auto other = sampleSave();
    other.name = "OTHER";
    save.autoAttack = false;
    CHECK(other.autoAttack);
    for (const bool enabled : {false, true}) {
        save.autoAttack = enabled;
        save.selectClass(0);
        CHECK(save.autoAttack == enabled);
        save.selectClass(0);
        CHECK(save.autoAttack == enabled);
        save.selectClass(2);
        CHECK(save.autoAttack == enabled);
        const auto loaded = CharacterSave::fromJson(save.toJson());
        CHECK(loaded.autoAttack == enabled);
        CHECK(loaded.toJson() == save.toJson());
    }
    CHECK(other.autoAttack);
}

TEST_CASE("save slots reload independent autoattack preferences",
          "[game][players][save][autoattack]") {
    const auto dir = test::scratchDirectory("save-autoattack");
    SaveSlots slots;
    REQUIRE(slots.open(dir, 2));
    auto save = sampleSave();
    save.autoAttack = false;
    REQUIRE(slots.write(0, save));
    save.name = "OTHER";
    save.autoAttack = true;
    REQUIRE(slots.write(1, save));

    SaveSlots reopened;
    REQUIRE(reopened.open(dir, 2));
    CharacterSave loaded;
    REQUIRE(reopened.load(0, loaded));
    CHECK_FALSE(loaded.autoAttack);
    REQUIRE(reopened.load(1, loaded));
    CHECK(loaded.autoAttack);
}

TEST_CASE("lifetime totals persist per class and old saves do not fabricate history",
          "[save][shop]") {
    auto save = sampleSave();
    save.progress().lifetime = {123, 45, 6789, 93780.5};
    const auto loaded = CharacterSave::fromJson(save.toJson());
    CHECK(loaded.progress().lifetime.enemiesKilled == 123);
    CHECK(loaded.progress().lifetime.generatorsDestroyed == 45);
    CHECK(loaded.progress().lifetime.goldFound == 6789);
    CHECK(loaded.progress().lifetime.playSeconds == 93780.5);
    CHECK(loaded.classes[0].lifetime.enemiesKilled == 0);
    const auto old = CharacterSave::fromJson(
        R"({"name":"OLD","character":0,"gold":500,"classes":{"WAR":{"experience":5000}}})");
    CHECK(old.progress().lifetime.goldFound == 0);
    CHECK(old.progress().lifetime.enemiesKilled == 0);
    CHECK(old.progress().lifetime.playSeconds == 0);
}

TEST_CASE("changing class banks each wallet without copying or discarding gold",
          "[game][players][save][class-wallet]") {
    // player_get_from_save / player_store_in_save use P_SAVE_STUFF[character].gold.
    CharacterSave save;
    save.gold = 1250;
    save.selectClass(0);
    CHECK(save.gold == 1250);
    save.selectClass(1);
    CHECK(save.gold == 0);
    CHECK(save.classes[0].gold == 1250);
    save.gold = 430;
    save.selectClass(0);
    CHECK(save.gold == 1250);
    CHECK(save.classes[1].gold == 430);
    save.gold -= 200;
    auto loaded = CharacterSave::fromJson(save.toJson());
    CHECK(loaded.gold == 1050);
    CHECK(loaded.progress().gold == 1050);
    CHECK(loaded.classes[1].gold == 430);
    loaded.selectClass(1);
    CHECK(loaded.gold == 430);
    loaded.selectClass(0);
    CHECK(loaded.gold == 1050);
    CHECK(loaded.toJson() == save.toJson());
}

TEST_CASE("legacy gold belongs only to the selected class and keeps top-level compatibility",
          "[game][players][save][class-wallet]") {
    auto save = CharacterSave::fromJson(
        R"({"version":1,"name":"OLD","character":2,"gold":723,"classes":{"WAR":{"experience":50}}})");
    CHECK(save.gold == 723);
    CHECK(save.classes[2].gold == 723);
    save.selectClass(0);
    CHECK(save.gold == 0);
    save.selectClass(2);
    CHECK(save.gold == 723);
    // Older readers/writers only know the live top-level balance. Never replace it
    // with a stale class checkpoint when opening one of their saves.
    save = CharacterSave::fromJson(
        R"({"name":"OLD","character":0,"gold":100,"classes":{"WAR":{"gold":900},"VAL":{"gold":50}}})");
    CHECK(save.gold == 100);
    CHECK(save.progress().gold == 100);
    save.selectClass(1);
    CHECK(save.gold == 50);
    save.selectClass(0);
    CHECK(save.gold == 100);
}

TEST_CASE("a character round-trips through JSON", "[game][players][save]") {
    const CharacterSave save = sampleSave();
    const CharacterSave loaded = CharacterSave::fromJson(save.toJson());
    REQUIRE(loaded.progress().crystals[1] == 15);
    // The levels beaten and the boss deaths come back whole; a save without them (and any
    // other class) has beaten nothing.
    REQUIRE(loaded.progress().levels == save.progress().levels);
    REQUIRE(loaded.progress().levels.hasBeaten(7, 2));
    REQUIRE(loaded.progress().levels.runeLevels == std::array<u16, 2>{1U << 7U, 1U << 7U});
    REQUIRE(loaded.progress().levels.bossDeaths == std::array<u16, 2>{1U << 2U, 0});
    REQUIRE(loaded.classes[9].levels == LevelRecord{});
    REQUIRE(loaded.progress().unlocked == 0b10);
    REQUIRE(loaded.progress().crystals[2] == 0);
    REQUIRE(loaded.name == "PELE");
    REQUIRE(loaded.character == 2);
    REQUIRE(loaded.color == 3);
    REQUIRE(loaded.classUnlock == 0x101);
    REQUIRE(loaded.gold == 1500);
    REQUIRE(loaded.levelTotal == 7);
    REQUIRE(loaded.experience() == 1060);
    REQUIRE(loaded.classes[2].fightAdd == 12.5f);
    REQUIRE(loaded.classes[9].health == 320);
    REQUIRE(loaded.classes[0].experience == 0);
    REQUIRE(loaded.progress().inventory == save.progress().inventory);
    REQUIRE(loaded.progress().inventory.keys == 4);
    REQUIRE(loaded.progress().inventory.nextPotion() == 3);
    REQUIRE(loaded.progress().inventory.powerup(9, 0x8000)->charge == 2.0f);
    REQUIRE_FALSE(loaded.progress().inventory.powerups[1].on);
    REQUIRE(loaded.progress().inventory.powerups[1].held());
    REQUIRE(loaded.classes[0].inventory == Inventory{});
    REQUIRE(loaded.toJson() == save.toJson());
    // A save from before inventories, or one overfull, loads within the limits.
    const CharacterSave old = CharacterSave::fromJson(
        R"({"version": 1, "name": "OLD", "character": 0, "classes": {"WAR": {"experience": 5,
            "inventory": {"keys": 40, "potions": [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]}},
            "VAL": {"experience": 1}}})");
    REQUIRE(old.classes[0].inventory.keys == Inventory::kMostKeys);
    REQUIRE(old.classes[0].inventory.potions.size() == 9);
    REQUIRE(old.classes[1].inventory == Inventory{});
    REQUIRE(old.classes[0].levels == LevelRecord{}); // from before the record: nothing beaten
}

TEST_CASE("pending and awarded promotions survive saves independently per class",
          "[game][players][save][promotion]") {
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = 29;
    save.classes[1].experience = levelExperience(80);
    save.classes[1].promotedLevel = 80;
    const auto loaded = CharacterSave::fromJson(save.toJson());
    REQUIRE(loaded.progress().appearanceLevel() == 29);
    REQUIRE(loaded.progress().promotionPending());
    REQUIRE(loaded.classes[1].appearanceLevel() == 80);
    REQUIRE_FALSE(loaded.classes[1].promotionPending());
    save.progress().promotedLevel = 30;
    REQUIRE_FALSE(CharacterSave::fromJson(save.toJson()).progress().promotionPending());
    // Legacy characters retain their existing appearance rather than replaying old awards.
    const auto old = CharacterSave::fromJson(
        R"({"version":1,"name":"OLD","character":0,"classes":{"WAR":{"experience":165200}}})");
    REQUIRE(old.progress().appearanceLevel() == 60);
    REQUIRE_FALSE(old.progress().promotionPending());
}

TEST_CASE("broken characters are rejected", "[game][players][save]") {
    REQUIRE_THROWS_AS(CharacterSave::fromJson("{nope"), FormatError);
    REQUIRE_THROWS_AS(CharacterSave::fromJson(R"({"name": "X"})"), FormatError);
    REQUIRE_THROWS_AS(CharacterSave::fromJson(R"({"name": "X", "character": 40})"), FormatError);
    REQUIRE_THROWS_AS(CharacterSave::fromJson(R"({"name": "TOOLONGNAME", "character": 1})"),
                      FormatError);
    REQUIRE_NOTHROW(CharacterSave::fromJson(R"({"name": "OK", "character": 1})"));
}

TEST_CASE("save slots list, write and read the directory", "[game][players][save]") {
    const auto dir = test::scratchDirectory("save-slots");
    SaveSlots slots;
    REQUIRE(slots.open(dir, 3));
    REQUIRE(slots.opened());
    REQUIRE(slots.count() == 3);
    REQUIRE_FALSE(slots.anySaved());
    REQUIRE_FALSE(slots.slot(1).exists);
    REQUIRE(slots.path(1).filename() == "slot2.json");

    REQUIRE(slots.write(1, sampleSave()));
    REQUIRE(slots.anySaved());
    REQUIRE(slots.slot(1).exists);
    REQUIRE(slots.slot(1).name == "PELE");
    REQUIRE(slots.slot(1).character == 2);
    REQUIRE_FALSE(slots.write(5, sampleSave()));

    CharacterSave loaded;
    REQUIRE_FALSE(slots.load(0, loaded));
    REQUIRE(slots.load(1, loaded));
    REQUIRE(loaded.gold == 1500);

    SaveSlots again;
    REQUIRE(again.open(dir, 3));
    REQUIRE(again.slot(1).exists);
    REQUIRE(again.slot(1).color == 3);

    writeTextFile(dir / "slot3.json", "garbage");
    again.refresh();
    REQUIRE_FALSE(again.slot(2).exists);
    REQUIRE(again.slot(1).exists);
}

TEST_CASE("a character keeps the help it has been shown, in order", "[game][players][save]") {
    CharacterSave save = sampleSave();
    save.helpSeen = {21, 2, 133};
    const CharacterSave loaded = CharacterSave::fromJson(save.toJson());
    REQUIRE(loaded.helpSeen == std::vector<s32>{2, 21, 133});
    REQUIRE(CharacterSave::fromJson(sampleSave().toJson()).helpSeen.empty());
}

TEST_CASE("legacy class rewards migrate without loading or rewriting a character",
          "[save][global-unlocks]") {
    const auto dir = test::scratchDirectory("class-unlock-migration");
    CharacterSave first;
    first.name = "FIRST";
    first.classUnlock = 1;
    CharacterSave second;
    second.name = "SECOND";
    second.classUnlock = 0x100;
    writeTextFile(dir / "slot1.json", first.toJson());
    writeTextFile(dir / "slot3.json", second.toJson());
    writeTextFile(dir / "slot2.json", "broken character");
    SaveSlots slots;
    REQUIRE(slots.open(dir, 3));
    CHECK(slots.classUnlocks() == 0x101);
    CHECK(readTextFile(dir / "slot1.json") == first.toJson());
    CHECK(readTextFile(dir / "slot3.json") == second.toJson());
    CHECK(readTextFile(dir / "slot2.json") == "broken character");
    // Overwriting the original owners cannot remove installation-wide availability.
    CharacterSave fresh;
    fresh.name = "NEW";
    REQUIRE(slots.write(0, fresh));
    REQUIRE(slots.write(2, fresh));
    SaveSlots restarted;
    REQUIRE(restarted.open(dir, 3));
    CHECK(restarted.classUnlocks() == 0x101);
    CharacterSave loaded;
    REQUIRE(restarted.load(0, loaded));
    CHECK(loaded.classUnlock == 0); // the character is not the authority
    CHECK(loaded.experience() == 0);
}

TEST_CASE("shared unlock rewards persist without a slot and older writers retain new rewards",
          "[save][global-unlocks]") {
    const auto dir = test::scratchDirectory("class-unlock-shared-writers");
    SaveSlots live;
    SaveSlots select;
    REQUIRE(live.open(dir, 2));
    REQUIRE(select.open(dir, 2));
    REQUIRE(live.unlockClasses(1));
    REQUIRE(select.unlockClasses(0x100));
    REQUIRE(live.unlockClasses(2));
    select.refresh();
    CHECK(select.classUnlocks() == 0x103);
    CHECK_FALSE(select.anySaved());
    SaveSlots restarted;
    REQUIRE(restarted.open(dir, 2));
    CHECK(restarted.classUnlocks() == 0x103);
    REQUIRE(restarted.unlockClasses(0));
    CHECK(restarted.classUnlocks() == 0x103);
    REQUIRE(restarted.open(test::scratchDirectory("class-unlock-other-install"), 2));
    CHECK(restarted.classUnlocks() == 0);
}

TEST_CASE("unreadable and future unlock profiles are preserved instead of overwritten",
          "[save][global-unlocks]") {
    const auto dir = test::scratchDirectory("class-unlock-invalid");
    for (const auto* text :
         {"broken", R"({"version":2,"classUnlock":1})", R"({"version":1,"classUnlock":-1})",
          R"({"version":1,"classUnlock":0.5})"}) {
        writeTextFile(dir / "unlocks.json", text);
        SaveSlots slots;
        REQUIRE(slots.open(dir, 1));
        CHECK_FALSE(slots.unlockClasses(2));
        CHECK(slots.classUnlocks() == 2); // usable for this session even on write failure
        CHECK(readTextFile(dir / "unlocks.json") == text);
    }
}

} // namespace
