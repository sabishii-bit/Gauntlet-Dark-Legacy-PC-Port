#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

/** The top-level ids a table names more than once, each repeat in turn: a parser keeps only
 * the last entry of an id, so the earlier ones are never shown. */
std::vector<std::string> repeatedIds(const std::vector<u8>& table) {
    std::set<std::string> seen;
    std::vector<std::string> repeated;
    const auto note = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        // The outermost object's keys arrive one level down.
        if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
            auto id = parsed.get<std::string>();
            if (!seen.insert(id).second) {
                repeated.push_back(std::move(id));
            }
        }
        return true;
    };
    (void)nlohmann::json::parse(table.begin(), table.end(), note);
    return repeated;
}

std::filesystem::path sampleTables() {
    const auto dir = test::scratchDirectory("string-tables");
    writeTextFile(dir / "en.json",
                  R"({"language.name": "English", "menu.start": "Start", "menu.back": "Back"})");
    writeTextFile(dir / "fr.json", R"({"menu.start": "Commencer"})");
    writeTextFile(dir / "bad.json", "{oops");
    writeTextFile(dir / "list.json", "[1, 2]");
    writeTextFile(dir / "notes.txt", "{}");
    return dir;
}

TEST_CASE("a translation overrides the fallback entry by entry", "[assets][text]") {
    const auto dir = sampleTables();
    StringTable table;
    REQUIRE(table.load(dir, "fr"));
    REQUIRE(table.language() == "fr");
    REQUIRE(table.size() == 3); // language.name comes with the fallback
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

TEST_CASE("another directory's tables lay over the loaded ones in the table's language",
          "[assets][text]") {
    const auto dir = sampleTables();
    const auto extra = test::scratchDirectory("string-tables-extra");
    writeTextFile(extra / "en.json", R"({"mod.name": "Rage", "menu.back": "Retreat"})");
    writeTextFile(extra / "fr.json", R"({"mod.name": "Rage!"})");
    StringTable table;
    REQUIRE(table.load(dir, "fr"));
    REQUIRE(table.layer(extra));
    CHECK(table.get("mod.name") == "Rage!");       // the table's language over its fallback
    CHECK(table.get("menu.back") == "Retreat");    // an id laid again replaces the loaded one
    CHECK(table.get("menu.start") == "Commencer"); // the rest as loaded
    CHECK(table.language() == "fr");
    // A directory holding neither table lays nothing.
    CHECK_FALSE(table.layer(test::scratchDirectory("string-tables-none")));
    CHECK(table.get("mod.name") == "Rage!");
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

TEST_CASE("every shipped text table names each identifier once", "[text]") {
    // Only the outermost object's ids count, and each repeat is named.
    const auto scratch = test::scratchDirectory("string-tables-repeats");
    writeTextFile(scratch / "en.json", R"({"a": "x", "b": {"a": "inner"}, "a": "y", "a": "z"})");
    CHECK(repeatedIds(readFile(scratch / "en.json")) == std::vector<std::string>{"a", "a"});
    usize tables = 0;
    for (const auto& entry : std::filesystem::directory_iterator(test::dataDirectory() / "text")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        ++tables;
        const auto repeated = repeatedIds(readFile(entry.path()));
        INFO(entry.path().filename().string());
        CAPTURE(repeated);
        CHECK(repeated.empty());
    }
    CHECK(tables > 0);
}

TEST_CASE("text identity hashes resolved entries not JSON formatting or insertion order",
          "[assets][text][native-online-identity]") {
    const auto firstDirectory = test::scratchDirectory("string-identity-first");
    const auto secondDirectory = test::scratchDirectory("string-identity-second");
    writeTextFile(firstDirectory / "en.json", R"({"a":"first","b":"fallback"})");
    writeTextFile(firstDirectory / "fr.json", R"({"a":"translation"})");
    writeTextFile(secondDirectory / "en.json", R"({"b":"fallback","a":"unused"})");
    writeTextFile(secondDirectory / "fr.json", R"({ "a" : "translation" })");
    StringTable first;
    StringTable second;
    REQUIRE(first.load(firstDirectory, "fr"));
    REQUIRE(second.load(secondDirectory, "fr"));
    CHECK(first.digest().size() == 64);
    CHECK(first.digest() == second.digest());

    writeTextFile(secondDirectory / "en.json", R"({"b":"changed","a":"unused"})");
    REQUIRE(second.load(secondDirectory, "fr"));
    CHECK(first.digest() != second.digest());
    writeTextFile(secondDirectory / "en.json", R"({"b":"fallback","a":"translation"})");
    REQUIRE(second.load(secondDirectory, "en"));
    CHECK(first.get("a") == second.get("a"));
    CHECK(first.get("b") == second.get("b"));
    CHECK(first.digest() != second.digest());
}

TEST_CASE(
    "text identity length-prefixes keys and values and hashes the effective fallback language",
    "[assets][text][native-online-identity]") {
    const auto directory = test::scratchDirectory("string-identity-lengths");
    writeTextFile(directory / "en.json", R"({"ab":"c"})");
    StringTable first;
    REQUIRE(first.load(directory, "en"));
    StringTable fallback;
    REQUIRE(fallback.load(directory, "missing"));
    CHECK(fallback.language() == "en");
    CHECK(fallback.digest() == first.digest());
    writeTextFile(directory / "en.json", R"({"a":"bc"})");
    StringTable second;
    REQUIRE(second.load(directory, "en"));
    CHECK(second.digest() != first.digest());
}

TEST_CASE("a directory's languages are its tables by code, named as they name themselves",
          "[assets][text][language]") {
    const auto dir = sampleTables();
    // Broken, non-object and non-JSON files are not languages; a table without a name goes
    // by its code; codes come in order.
    const std::vector<StringTable::Language> expected{{"en", "English"}, {"fr", "fr"}};
    REQUIRE(StringTable::available(dir) == expected);
    StringTable table;
    REQUIRE(table.load(dir, "fr"));
    REQUIRE(table.languages() == expected);
    REQUIRE(
        StringTable::available(test::scratchDirectory("string-tables-none") / "missing").empty());
    REQUIRE(StringTable{}.languages().empty());
}

} // namespace
