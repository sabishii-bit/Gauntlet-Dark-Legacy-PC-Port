#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/console/ConsoleCommands.h"
#include "game/console/ConsoleUsage.h"
#include "game/console/FixtureRequest.h"
#include "game/console/GiftRequest.h"
#include "game/console/PartyCountRequest.h"
#include "game/console/SpawnRequest.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Row = std::pair<std::string_view, std::string_view>;

TEST_CASE("usage lines put the forms in a column with what each does beside them",
          "[dev-console][commands]") {
    for (const std::string& text :
         {spawnUsage(), giftUsage(), ConsoleCommands::gotoUsage(), lockUsage(LockVerb::Lock),
          lockUsage(LockVerb::Unlock), activateUsage(), partyCountUsage()}) {
        CAPTURE(text);
        std::vector<std::string> lines;
        for (usize start = 0; start <= text.size();) {
            const usize end = std::min(text.find('\n', start), text.size());
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        REQUIRE(lines.size() >= 2);
        // Every form line's description starts at the same column, past the padding.
        const auto descriptionColumn = [](const std::string& line) {
            const usize gap = line.find("  ");
            return gap == std::string::npos ? std::string::npos : line.find_first_not_of(' ', gap);
        };
        const usize column = descriptionColumn(lines[0]);
        REQUIRE(column != std::string::npos);
        const std::string command = lines[0].substr(0, lines[0].find(' '));
        for (const std::string& line : lines) {
            if (line.starts_with(command)) {
                CHECK(descriptionColumn(line) == column);
                CHECK(line.size() > column);
            }
        }
    }
    CHECK(spawnUsage().contains("spawn object <name> [map]"));
    CHECK(spawnUsage().contains("spawn item <name>"));
}

TEST_CASE("a usage pads every form to the widest and sets its notes beneath",
          "[dev-console][commands]") {
    constexpr std::array<Row, 3> kRows{{{"say", "speaks"}, {"say loud", "shouts"}, {"s", "hums"}}};
    constexpr std::array<std::string_view, 2> kNotes{"First note.", "Second note."};
    CHECK(consoleUsage(kRows, kNotes) == "say       speaks\n"
                                         "say loud  shouts\n"
                                         "s         hums\n"
                                         "First note.\n"
                                         "Second note.");
    CHECK(consoleUsage(std::span<const Row>(kRows).first(1)) == "say  speaks");
    CHECK(consoleUsage({}).empty());
}

} // namespace
