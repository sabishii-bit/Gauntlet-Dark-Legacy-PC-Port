#include <catch2/catch_test_macros.hpp>

#include "game/players/CharacterSave.h"
#include "game/players/Relics.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("relics keep the runes, legend items and gargoyle pieces within their bounds",
          "[game][players]") {
    Relics relics;
    REQUIRE(relics.runeCount() == 0);
    REQUIRE(relics.addRune(0));
    REQUIRE(relics.addRune(12));
    REQUIRE_FALSE(relics.addRune(12));   // held already
    REQUIRE_FALSE(relics.addRune(13));   // no such rune
    REQUIRE_FALSE(relics.addRune(-1));
    REQUIRE(relics.runeCount() == 2);
    REQUIRE(relics.hasRune(0));
    REQUIRE_FALSE(relics.hasRune(5));
    REQUIRE_FALSE(relics.hasRune(13));
    // A legend item is held until spent on its boss.
    REQUIRE(relics.addLegend(11));
    REQUIRE(relics.hasLegend(11));
    REQUIRE_FALSE(relics.spendLegend(2));
    REQUIRE(relics.spendLegend(11));
    REQUIRE_FALSE(relics.hasLegend(11));
    REQUIRE_FALSE(relics.addLegend(16));
    // Gargoyle pieces count up to the statues' want and no further.
    REQUIRE(relics.addGargoylePiece(2) == 1);
    for (int i = 0; i < 40; ++i) {
        relics.addGargoylePiece(2);
    }
    REQUIRE(relics.gargoylePieces[2] == 28);
    REQUIRE(relics.gargoyleComplete(2));
    REQUIRE_FALSE(relics.gargoyleComplete(1));
    REQUIRE(relics.addGargoylePiece(3) == -1);
    REQUIRE_FALSE(relics.gargoyleComplete(3));
}

TEST_CASE("relics survive the save's round trip", "[game][players]") {
    CharacterSave save;
    save.name = "AB";
    Relics& relics = save.progress().relics;
    relics.addRune(3);
    relics.addRune(9);
    relics.addLegend(4);
    relics.addGargoylePiece(0);
    relics.addGargoylePiece(0);
    const CharacterSave back = CharacterSave::fromJson(save.toJson());
    REQUIRE(back.progress().relics == relics);
    REQUIRE(back.progress().relics.runeCount() == 2);
    REQUIRE(back.progress().relics.hasLegend(4));
    REQUIRE(back.progress().relics.gargoylePieces[0] == 2);
    // An older save without them reads as none gathered.
    const CharacterSave older = CharacterSave::fromJson(
        R"({"name": "CD", "character": 0, "classes": {"WAR": {"experience": 10}}})");
    REQUIRE(older.progress().experience == 10);
    REQUIRE(older.progress().relics == Relics{});
}

} // namespace
