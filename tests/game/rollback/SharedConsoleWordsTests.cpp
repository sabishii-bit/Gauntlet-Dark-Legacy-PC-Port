#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/rollback/SharedConsoleWords.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the shared words are found by their lower-case names, in the console's order",
          "[netplay][console-input][online-console]") {
    constexpr std::array<std::string_view, 8> kNames{"goto",    "unlock_tower", "get", "spawn",
                                                     "suicide", "notarget",     "god", "noclip"};
    REQUIRE(kSharedConsoleWords.size() == kNames.size());
    for (usize i = 0; i < kNames.size(); ++i) {
        CAPTURE(kNames[i]);
        CHECK(kSharedConsoleWords[i].name == kNames[i]);
        CHECK(findSharedConsoleWord(kNames[i]) == &kSharedConsoleWords[i]);
        CHECK(kSharedConsoleWords[i].localForm == (kNames[i] == "get" ? "info" : ""));
    }
    STATIC_REQUIRE(findSharedConsoleWord("get")->localForm == "info");
    for (const std::string_view other : {"GOD", "Get", "help", "list", "", "go", "gods"}) {
        CAPTURE(other);
        CHECK(findSharedConsoleWord(other) == nullptr);
    }
}

TEST_CASE("a console line's words are split on spaces alone",
          "[netplay][console-input][online-console]") {
    struct Line {
        std::string_view line;
        std::vector<std::string> words;
    };
    const std::array<Line, 11> lines{{{"", {}},
                                      {"   ", {}},
                                      {"god", {"god"}},
                                      {"  god", {"god"}},
                                      {"god  ", {"god"}},
                                      {"god  x", {"god", "x"}},
                                      {"spawn Gru 2  3 ", {"spawn", "Gru", "2", "3"}},
                                      {" a  b   c ", {"a", "b", "c"}},
                                      {"\tx y", {"\tx", "y"}},
                                      {"get info enemy#3", {"get", "info", "enemy#3"}},
                                      {"x\t y", {"x\t", "y"}}}};
    for (const Line& line : lines) {
        CAPTURE(line.line);
        CHECK(splitConsoleWords(line.line) == line.words);
    }
}

} // namespace
