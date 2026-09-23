#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
  ],
  "lists": [{"name": "GREETINGS", "messages": [1, 0, 9]}]
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

TEST_CASE("a message table's lists name messages in their own order", "[assets][text]") {
    MessageTable table;
    REQUIRE(table.load(sampleTable("message-table-lists")));
    REQUIRE(table.lists().size() == 1);
    REQUIRE(table.findList("NOPE") == nullptr);
    const MessageList* greetings = table.findList("GREETINGS");
    REQUIRE(greetings != nullptr);
    REQUIRE(greetings->messages == std::vector<std::int32_t>{1, 0, 9});
    REQUIRE(table.listed(*greetings, 1) == &table.message(0));
    REQUIRE(table.listed(*greetings, 2) == nullptr); // names no message of the table
    REQUIRE(table.listed(*greetings, 3) == nullptr); // past the list's end
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
    const auto path = test::unpackedOrSkip("text/scroll_e.json");
    REQUIRE(table.load(path));
    const auto index = table.find("WELCOMEMESSAGE");
    REQUIRE(index.has_value());
    const MessageInfo& welcome = table.message(*index);
    REQUIRE(welcome.pages.size() == 5);
    REQUIRE(welcome.pages[0].starts_with("Welcome, mighty heroes!"));
    REQUIRE(welcome.scale == Approx(0.6f));
    REQUIRE(table.fontOf(welcome) == "font32");
}

TEST_CASE("a message's pages come from the string table when it has them", "[assets][text]") {
    REQUIRE(MessageTable::textId("scroll", "HELLO", 2) == "scroll.hello.2");
    MessageTable table;
    REQUIRE(table.load(sampleTable("message-table-translate")));
    const auto dir = test::scratchDirectory("message-table-strings");
    writeTextFile(dir / "xx.json", R"({
  "scroll.hello.1": "Bienvenue,\nheros !",
  "scroll.hello.2": "Allez.",
  "scroll.hello.3": "Vite !",
  "scroll.hello.5": "never reached: four is missing",
  "scroll.odd.2": "no first page, so not taken",
  "other.hello.1": "another table's"
})");
    StringTable strings;
    REQUIRE(strings.load(dir, "xx", "xx"));
    REQUIRE(table.translate(strings, "scroll") == 1);
    const MessageInfo& hello = table.message(*table.find("HELLO"));
    REQUIRE(hello.pages == std::vector<std::string>{"Bienvenue,\nheros !", "Allez.", "Vite !"});
    REQUIRE(hello.scale == Approx(0.6f)); // how it is set stays the rom's
    REQUIRE(table.message(*table.find("ODD")).pages.empty());
    // A table the strings know nothing of keeps its own words.
    MessageTable untouched;
    REQUIRE(untouched.load(sampleTable("message-table-untouched")));
    REQUIRE(untouched.translate(strings, "hint") == 0);
    REQUIRE(untouched.message(0).pages.size() == 2);
}

TEST_CASE("the shipped English has every scroll, hint and help message, word for word",
          "[assets][text][unpacked]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en", "en"));
    struct Rom {
        const char* file;
        const char* prefix;
        bool whole; ///< every message of it, or only those the strings name
    };
    for (const Rom& rom :
         {Rom{"text/scroll_e.json", "scroll", true}, Rom{"text/hints_e.json", "hint", true},
          Rom{"text/english.json", "help", false}}) {
        const auto path = test::unpackedOrSkip(rom.file);
        MessageTable original;
        REQUIRE(original.load(path));
        MessageTable translated;
        REQUIRE(translated.load(path));
        const std::size_t taken = translated.translate(strings, rom.prefix);
        REQUIRE(taken > 0);
        if (rom.whole) {
            REQUIRE(taken == original.size());
        }
        for (std::uint32_t i = 0; i < original.size(); ++i) {
            REQUIRE(translated.message(i).pages == original.message(i).pages);
        }
    }
}

} // namespace
