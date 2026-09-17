#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
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
    return save;
}

TEST_CASE("a character round-trips through JSON", "[game][players][save]") {
    const CharacterSave save = sampleSave();
    const CharacterSave loaded = CharacterSave::fromJson(save.toJson());
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
    REQUIRE(loaded.toJson() == save.toJson());
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

} // namespace
