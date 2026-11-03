#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Strings.h"
#include "engine/core/Types.h"

#include "game/rollback/ConsoleAction.h"
#include "game/rollback/SharedConsoleWords.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Line {
    std::string_view line;
    std::vector<std::string> words;
};

/** Lines and the words splitConsoleWords reads from them. */
std::array<Line, 11> splitLines() {
    return {{{"", {}},
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
}

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
    for (const Line& line : splitLines()) {
        CAPTURE(line.line);
        CHECK(splitConsoleWords(line.line) == line.words);
    }
}

TEST_CASE("the wire reads a line's verb and local form as the split words in lower case",
          "[netplay][console-input][online-console]") {
    std::vector<std::string> lines;
    for (const Line& line : splitLines()) {
        lines.emplace_back(line.line);
    }
    for (const std::string_view line :
         {"GOD", "  NoClip  ", "Goto G1", "Get INFO x", "get Info", "GET infos", "get  info",
          "Spawn Gru 2", "gods", "get", "unlock_tower x"}) {
        lines.emplace_back(line);
    }
    for (const std::string& line : lines) {
        CAPTURE(line);
        const std::vector<std::string> words = splitConsoleWords(line);
        const SharedConsoleWord* verb =
            words.empty() ? nullptr : findSharedConsoleWord(toLowerAscii(words.front()));
        const bool local = verb != nullptr && !verb->localForm.empty() && words.size() > 1 &&
                           toLowerAscii(words[1]) == verb->localForm;
        // A tab is the only unprintable character these lines hold.
        const bool printable = line.find('\t') == std::string::npos;
        CHECK(ConsoleAction{0, line}.admitted() == (printable && verb != nullptr && !local));
    }
}

} // namespace
