#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/MessageTable.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

std::filesystem::path sampleTable(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "scroll.json", R"({
  "fonts": ["font32"],
  "messages": [
    {"name": "HELLO", "font": 0, "scale": 0.6, "shadowScale": 1,
     "lines": ["Welcome,\r\nheroes!\n", "Go now."]},
    {"name": "ODD", "font": 3, "lines": []}
  ]
})");
    return dir / "scroll.json";
}

TEST_CASE("a message table names its messages, their pages and fonts", "[assets][text]") {
    MessageTable table;
    REQUIRE_FALSE(table.loaded());
    REQUIRE(table.load(sampleTable("message-table")));
    REQUIRE(table.loaded());
    REQUIRE(table.size() == 2);
    REQUIRE(table.fonts() == std::vector<std::string>{"font32"});
    REQUIRE(table.find("HELLO") == 0U);
    REQUIRE_FALSE(table.find("NOPE").has_value());
    const MessageInfo& hello = table.message(0);
    REQUIRE(hello.scale == Approx(0.6f));
    REQUIRE(hello.pages.size() == 2);
    REQUIRE(hello.pages[0] == "Welcome,\nheroes!\n"); // carriage returns dropped
    REQUIRE(hello.pages[1] == "Go now.");
    REQUIRE(table.fontOf(hello) == "font32");
    REQUIRE(table.fontOf(table.message(1)).empty());
    REQUIRE(table.message(1).pages.empty());
}

TEST_CASE("a missing or malformed message table fails to load", "[assets][text]") {
    MessageTable table;
    REQUIRE_FALSE(table.load(test::scratchDirectory("message-table-none") / "none.json"));
    const auto dir = test::scratchDirectory("message-table-bad");
    writeTextFile(dir / "bad.json", R"({"fonts": []})");
    REQUIRE_FALSE(table.load(dir / "bad.json"));
    REQUIRE_FALSE(table.loaded());
}

TEST_CASE("the unpacked scroll texts hold Sumner's welcome", "[assets][text][unpacked]") {
    MessageTable table;
    REQUIRE(table.load(test::unpackedOrSkip("text/scroll_e.json")));
    const auto index = table.find("WELCOMEMESSAGE");
    REQUIRE(index.has_value());
    const MessageInfo& welcome = table.message(*index);
    REQUIRE(welcome.pages.size() == 5);
    REQUIRE(welcome.pages[0].starts_with("Welcome, mighty heroes!"));
    REQUIRE(welcome.scale == Approx(0.6f));
    REQUIRE(table.fontOf(welcome) == "font32");
}

} // namespace
