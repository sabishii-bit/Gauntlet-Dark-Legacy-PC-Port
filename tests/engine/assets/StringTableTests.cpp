#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

std::filesystem::path sampleTables() {
    const auto dir = test::scratchDirectory("string-tables");
    writeTextFile(dir / "en.json", R"({"menu.start": "Start", "menu.back": "Back"})");
    writeTextFile(dir / "fr.json", R"({"menu.start": "Commencer"})");
    writeTextFile(dir / "bad.json", "{oops");
    return dir;
}

TEST_CASE("a translation overrides the fallback entry by entry", "[assets][text]") {
    const auto dir = sampleTables();
    StringTable table;
    REQUIRE(table.load(dir, "fr"));
    REQUIRE(table.language() == "fr");
    REQUIRE(table.size() == 2);
    REQUIRE(table.get("menu.start") == "Commencer");
    REQUIRE(table.get("menu.back") == "Back");
    REQUIRE(table.has("menu.back"));
    REQUIRE_FALSE(table.has("menu.none"));
}

TEST_CASE("unknown identifiers come back as themselves", "[assets][text]") {
    const auto dir = sampleTables();
    StringTable table;
    REQUIRE(table.load(dir, "en"));
    REQUIRE(table.get("menu.none") == "menu.none");
}

TEST_CASE("a missing language falls back and a missing directory fails", "[assets][text]") {
    const auto dir = sampleTables();
    StringTable table;
    REQUIRE(table.load(dir, "de"));
    REQUIRE(table.get("menu.start") == "Start");
    REQUIRE(table.load(dir, "bad"));
    REQUIRE(table.get("menu.start") == "Start");
    REQUIRE_FALSE(table.load(test::scratchDirectory("string-tables-empty"), "en"));
    REQUIRE_FALSE(table.loaded());
}

TEST_CASE("the shipped English table names the title screen text", "[assets][text]") {
    const std::filesystem::path dir = test::dataDirectory() / "text";
    if (!std::filesystem::exists(dir / "en.json")) {
        SKIP("data/text/en.json is not available");
    }
    StringTable table;
    REQUIRE(table.load(dir, "en"));
    REQUIRE(table.get("title.pressStart") == "Press Start");
    REQUIRE(table.get("menu.options") == "Options");
    REQUIRE(table.get("menu.player") == "Player {}");
}

} // namespace
